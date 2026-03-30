from __future__ import annotations

import json
import logging
import threading
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from sqlalchemy import select

from .config import settings
from .db import session_scope
from .models import Device, DeviceFermentationConfig, DeviceHeartbeat, DeviceTelemetry, DiscoveredRelay

logger = logging.getLogger(__name__)


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _output_state_from_value(value) -> str | None:
    if isinstance(value, bool):
        return "on" if value else "off"
    if isinstance(value, str):
        if value in {"on", "off", "unknown"}:
            return value
        if value in {"true", "false"}:
            return "on" if value == "true" else "off"
    return None


class MqttBridge:
    def __init__(self) -> None:
        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="brewesp-web")
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.on_disconnect = self._on_disconnect
        if settings.mqtt_username:
            self._client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
        self._lock = threading.Lock()
        self._started = False

    def start(self) -> None:
        with self._lock:
            if self._started:
                return
            self._client.connect_async(settings.mqtt_host, settings.mqtt_port, 60)
            self._client.loop_start()
            self._started = True
            logger.info("MQTT bridge starting host=%s port=%s", settings.mqtt_host, settings.mqtt_port)

    def stop(self) -> None:
        with self._lock:
            if not self._started:
                return
            self._client.loop_stop()
            self._client.disconnect()
            self._started = False

    def publish_system_config(self, device_id: str, payload: dict) -> None:
        self._publish(f"{settings.mqtt_topic_prefix}/{device_id}/system_config", payload, retain=True)

    def publish_fermentation_config(self, device_id: str, payload: dict) -> None:
        self._publish(f"{settings.mqtt_topic_prefix}/{device_id}/config/desired", payload, retain=True)

    def publish_command(self, device_id: str, payload: dict) -> None:
        self._publish(f"{settings.mqtt_topic_prefix}/{device_id}/command", payload, retain=False)

    def _publish(self, topic: str, payload: dict, retain: bool) -> None:
        body = json.dumps(payload)
        with self._lock:
            info = self._client.publish(topic, body, qos=1, retain=retain)
        logger.info("MQTT publish topic=%s retain=%s rc=%s", topic, retain, info.rc)

    def _on_connect(self, client: mqtt.Client, userdata, flags, reason_code, properties) -> None:
        if reason_code != 0:
            logger.warning("MQTT connect failed rc=%s", reason_code)
            return

        prefix = settings.mqtt_topic_prefix
        for suffix in ("availability", "heartbeat", "state", "telemetry", "discovery/kasa", "config/applied"):
            topic = f"{prefix}/+/{suffix}"
            client.subscribe(topic)
        logger.info("MQTT bridge subscribed on prefix=%s", prefix)

    def _on_disconnect(self, client: mqtt.Client, userdata, flags, reason_code, properties) -> None:
        logger.warning("MQTT disconnected rc=%s", reason_code)

    def _on_message(self, client: mqtt.Client, userdata, msg: mqtt.MQTTMessage) -> None:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            logger.warning("Ignoring invalid JSON on topic=%s", msg.topic)
            return

        parts = msg.topic.split("/")
        if len(parts) < 3:
            return

        topic_type = parts[-1]
        if topic_type == "availability":
            self._handle_availability(payload)
        elif topic_type == "heartbeat":
            self._handle_heartbeat(payload)
        elif topic_type == "state":
            self._handle_state(payload)
        elif topic_type == "telemetry":
            self._handle_telemetry(payload)
        elif topic_type == "applied" and len(parts) >= 4 and parts[-2] == "config":
            self._handle_config_applied(payload)
        elif topic_type == "kasa" and len(parts) >= 4 and parts[-2] == "discovery":
            self._handle_kasa_discovery(payload)

    def _get_or_create_device(self, session, device_id: str) -> Device:
        device = session.scalar(select(Device).where(Device.device_id == device_id))
        if device:
            return device

        device = Device(device_id=device_id)
        session.add(device)
        session.flush()
        return device

    def _handle_availability(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        if not device_id:
            return

        with session_scope() as session:
            device = self._get_or_create_device(session, device_id)
            device.status = payload.get("status", "unknown")
            device.mqtt_connected = device.status == "online"
            device.fw_version = payload.get("fw_version", device.fw_version)
            device.last_seen_at = utcnow()

    def _handle_heartbeat(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        if not device_id:
            return

        with session_scope() as session:
            device = self._get_or_create_device(session, device_id)
            now = utcnow()
            device.status = "online"
            device.mqtt_connected = True
            device.last_seen_at = now
            device.last_heartbeat_at = now
            device.last_rssi = payload.get("wifi_rssi")
            device.last_heap_free = payload.get("heap_free")
            device.ui_mode = payload.get("ui")
            device.heating_state = payload.get("heating")
            device.cooling_state = payload.get("cooling")

            heartbeat = DeviceHeartbeat(
                device_id=device.id,
                recorded_at=now,
                uptime_s=payload.get("uptime_s"),
                wifi_rssi=payload.get("wifi_rssi"),
                heap_free=payload.get("heap_free"),
                ui_mode=payload.get("ui"),
                heating_state=payload.get("heating"),
                cooling_state=payload.get("cooling"),
                payload=payload,
            )
            session.add(heartbeat)

    def _handle_state(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        if not device_id:
            return

        with session_scope() as session:
            device = self._get_or_create_device(session, device_id)
            now = utcnow()
            device.last_state_at = now
            device.last_seen_at = now
            device.last_payload = payload
            device.heating_state = _output_state_from_value(payload.get("heating")) or device.heating_state
            device.cooling_state = _output_state_from_value(payload.get("cooling")) or device.cooling_state
            device.fw_version = payload.get("fw_version", device.fw_version)
            device.last_target_temp_c = payload.get(
                "effective_target_c",
                payload.get("setpoint_c", device.last_target_temp_c),
            )
            device.last_mode = payload.get("mode", device.last_mode)

    def _handle_telemetry(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        if not device_id:
            return

        with session_scope() as session:
            device = self._get_or_create_device(session, device_id)
            now = utcnow()
            device.last_seen_at = now
            device.last_temp_c = payload.get("temp_primary_c")
            device.last_secondary_temp_c = payload.get("temp_secondary_c")
            device.last_target_temp_c = payload.get(
                "effective_target_c",
                payload.get("setpoint_c", device.last_target_temp_c),
            )
            device.last_mode = payload.get("mode", device.last_mode)
            device.heating_state = _output_state_from_value(payload.get("heating")) or device.heating_state
            device.cooling_state = _output_state_from_value(payload.get("cooling")) or device.cooling_state

            sample = DeviceTelemetry(
                device_id=device.id,
                recorded_at=now,
                temp_primary_c=payload.get("temp_primary_c"),
                temp_secondary_c=payload.get("temp_secondary_c"),
                setpoint_c=payload.get("setpoint_c"),
                effective_target_c=payload.get("effective_target_c"),
                mode=payload.get("mode"),
                profile_id=payload.get("profile_id"),
                profile_step_id=payload.get("profile_step_id"),
                heating_active=bool(payload["heating"]) if isinstance(payload.get("heating"), bool) else None,
                cooling_active=bool(payload["cooling"]) if isinstance(payload.get("cooling"), bool) else None,
                payload=payload,
            )
            session.add(sample)

    def _handle_kasa_discovery(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        result = payload.get("result")
        if not device_id or not isinstance(result, dict):
            return

        host = result.get("host")
        if not host:
            return

        with session_scope() as session:
            source_device = self._get_or_create_device(session, device_id)
            relay = session.scalar(select(DiscoveredRelay).where(DiscoveredRelay.host == host))
            if relay is None:
                relay = DiscoveredRelay(host=host)
                session.add(relay)

            relay.source_device_id = source_device.id
            relay.driver = result.get("driver", relay.driver or "kasa_local")
            relay.port = int(result.get("port", relay.port or 9999))
            relay.alias = result.get("alias")
            relay.model = result.get("model")
            relay.is_on = result.get("is_on")
            relay.last_seen_at = utcnow()
            relay.raw_payload = result

    def _handle_config_applied(self, payload: dict) -> None:
        device_id = payload.get("device_id")
        if not device_id:
            return

        with session_scope() as session:
            device = self._get_or_create_device(session, device_id)
            config = session.scalar(
                select(DeviceFermentationConfig).where(DeviceFermentationConfig.device_id == device.id)
            )
            if config is None:
                config = DeviceFermentationConfig(device_id=device.id)
                session.add(config)

            config.last_applied_version = payload.get("applied_version")
            config.last_applied_result = payload.get("result")
            config.last_applied_message = payload.get("message")
            config.last_applied_at = utcnow()
