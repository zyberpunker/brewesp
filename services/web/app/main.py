from __future__ import annotations

from contextlib import asynccontextmanager
from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from sqlalchemy import desc, select
from sqlalchemy.orm import selectinload

from .config import settings
from .db import SessionLocal, init_db
from .models import (
    Device,
    DeviceFermentationConfig,
    DeviceHeartbeat,
    DeviceOutputAssignment,
    DeviceTelemetry,
    DiscoveredRelay,
)
from .mqtt_bridge import MqttBridge

templates = Jinja2Templates(directory="app/templates")
mqtt_bridge = MqttBridge()


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    mqtt_bridge.start()
    yield
    mqtt_bridge.stop()


app = FastAPI(title=settings.app_title, lifespan=lifespan)
app.mount("/static", StaticFiles(directory="app/static"), name="static")

STALE_AFTER_SECONDS = 90
OFFLINE_AFTER_SECONDS = 180


def _firmware_file_path(filename: str | None = None) -> Path:
    return settings.firmware_dir / (filename or settings.firmware_filename)


def _firmware_download_url(request: Request, filename: str) -> str:
    if settings.firmware_base_url:
        return f"{settings.firmware_base_url.rstrip('/')}/firmware/files/{filename}"
    return str(request.url_for("firmware_file", filename=filename))


def _firmware_sha256(path: Path) -> str:
    digest = sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _serialize_heartbeats(heartbeats: list[DeviceHeartbeat]) -> list[dict]:
    return [
        {
            "recorded_at": heartbeat.recorded_at.isoformat(),
            "wifi_rssi": heartbeat.wifi_rssi,
            "heap_free": heartbeat.heap_free,
            "uptime_s": heartbeat.uptime_s,
        }
        for heartbeat in heartbeats
    ]


def _serialize_telemetry(samples: list[DeviceTelemetry]) -> list[dict]:
    return [
        {
            "recorded_at": sample.recorded_at.isoformat(),
            "temp_primary_c": sample.temp_primary_c,
            "temp_secondary_c": sample.temp_secondary_c,
            "setpoint_c": sample.setpoint_c,
            "mode": sample.mode,
            "profile_id": sample.profile_id,
            "heating_active": sample.heating_active,
            "cooling_active": sample.cooling_active,
        }
        for sample in samples
    ]


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _apply_display_status(device: Device, now: datetime) -> Device:
    reference = device.last_heartbeat_at or device.last_seen_at
    display_status = "unknown"

    if reference is not None:
        age_seconds = (now - reference).total_seconds()
        if age_seconds <= STALE_AFTER_SECONDS:
            display_status = "online"
        elif age_seconds <= OFFLINE_AFTER_SECONDS:
            display_status = "stale"
        else:
            display_status = "offline"
    elif device.status in {"online", "offline"}:
        display_status = device.status

    device.display_status = display_status
    return device


def _output_button_state(current_state: str | None, expected: str) -> str:
    if current_state == expected:
        return "active"
    if current_state in {"on", "off"}:
        return "inactive"
    return "unknown"


def _device_live_payload(device: Device, now: datetime) -> dict:
    device = _apply_display_status(device, now)
    fermentation_config = device.fermentation_config
    last_payload = device.last_payload or {}
    return {
        "device_id": device.device_id,
        "display_status": device.display_status,
        "mqtt_connected": device.mqtt_connected,
        "last_heartbeat_at": device.last_heartbeat_at.isoformat() if device.last_heartbeat_at else None,
        "last_heartbeat_label": _format_timestamp(device.last_heartbeat_at),
        "heating_state": device.heating_state or "unknown",
        "cooling_state": device.cooling_state or "unknown",
        "last_temp_c": device.last_temp_c,
        "last_target_temp_c": device.last_target_temp_c,
        "heating_on_button_state": _output_button_state(device.heating_state, "on"),
        "heating_off_button_state": _output_button_state(device.heating_state, "off"),
        "cooling_on_button_state": _output_button_state(device.cooling_state, "on"),
        "cooling_off_button_state": _output_button_state(device.cooling_state, "off"),
        "last_mode": device.last_mode,
        "controller_state": last_payload.get("controller_state"),
        "controller_reason": last_payload.get("controller_reason"),
        "automatic_control_active": last_payload.get("automatic_control_active"),
        "beer_probe_present": last_payload.get("beer_probe_present"),
        "beer_probe_valid": last_payload.get("beer_probe_valid"),
        "beer_probe_rom": last_payload.get("beer_probe_rom"),
        "chamber_probe_present": last_payload.get("chamber_probe_present"),
        "chamber_probe_valid": last_payload.get("chamber_probe_valid"),
        "chamber_probe_rom": last_payload.get("chamber_probe_rom"),
        "secondary_sensor_enabled": last_payload.get("secondary_sensor_enabled"),
        "control_sensor": last_payload.get("control_sensor"),
        "fermentation_config": {
            "desired_version": fermentation_config.desired_version if fermentation_config else None,
            "last_applied_version": fermentation_config.last_applied_version if fermentation_config else None,
            "last_applied_result": fermentation_config.last_applied_result if fermentation_config else None,
            "last_applied_message": fermentation_config.last_applied_message if fermentation_config else None,
            "last_applied_at": fermentation_config.last_applied_at.isoformat()
            if fermentation_config and fermentation_config.last_applied_at
            else None,
            "last_applied_at_label": _format_timestamp(fermentation_config.last_applied_at)
            if fermentation_config
            else "never",
        },
    }


def _build_system_config_payload(device: Device) -> dict:
    assignments = {assignment.role: assignment for assignment in device.output_assignments}

    def _serialize(role: str) -> dict:
        assignment = assignments.get(role)
        if assignment is None or not assignment.host:
            return {
                "driver": "none",
                "host": "",
                "port": 9999,
                "alias": "",
            }

        return {
            "driver": assignment.driver,
            "host": assignment.host,
            "port": assignment.port,
            "alias": assignment.alias or "",
        }

    return {
        "device_id": device.device_id,
        "heating": _serialize("heating"),
        "cooling": _serialize("cooling"),
    }


def _build_fermentation_config_payload(device: Device, config: DeviceFermentationConfig) -> dict:
    return {
        "schema_version": config.schema_version,
        "version": config.desired_version,
        "device_id": device.device_id,
        "name": config.name or "Default fermentation",
        "mode": config.mode,
        "thermostat": {
            "setpoint_c": config.setpoint_c,
            "hysteresis_c": config.hysteresis_c,
            "cooling_delay_s": config.cooling_delay_s,
            "heating_delay_s": config.heating_delay_s,
        },
        "sensors": {
            "primary_offset_c": config.primary_offset_c,
            "secondary_enabled": config.secondary_enabled,
            "secondary_offset_c": config.secondary_offset_c if config.secondary_enabled else 0.0,
            "secondary_limit_hysteresis_c": (
                config.secondary_limit_hysteresis_c if config.secondary_enabled else 1.5
            ),
            "control_sensor": config.control_sensor,
        },
        "alarms": {
            "deviation_c": 2.0,
        },
    }


def _upsert_assignment(session, device: Device, role: str, relay: DiscoveredRelay | None) -> None:
    existing = next((assignment for assignment in device.output_assignments if assignment.role == role), None)
    if existing is None:
        existing = DeviceOutputAssignment(device_id=device.id, role=role)
        device.output_assignments.append(existing)
        session.add(existing)

    if relay is None:
        existing.driver = "none"
        existing.host = None
        existing.port = 9999
        existing.alias = None
        return

    existing.driver = relay.driver
    existing.host = relay.host
    existing.port = relay.port
    existing.alias = relay.alias


def _get_or_create_fermentation_config(session, device: Device) -> DeviceFermentationConfig:
    config = device.fermentation_config
    if config is None:
        config = DeviceFermentationConfig(device_id=device.id)
        device.fermentation_config = config
        session.add(config)
    return config


def _format_temperature(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.1f}°C"


def _format_timestamp(value: datetime | None) -> str:
    if value is None:
        return "never"
    return value.astimezone().strftime("%Y-%m-%d %H:%M:%S")


@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request):
    now = _utcnow()
    with SessionLocal() as session:
        devices = session.scalars(
            select(Device)
            .options(
                selectinload(Device.heartbeats),
                selectinload(Device.output_assignments),
                selectinload(Device.fermentation_config),
            )
            .order_by(desc(Device.last_seen_at))
        ).unique().all()
        devices = [_apply_display_status(device, now) for device in devices]

        telemetry_series = {}
        for device in devices:
            samples = session.scalars(
                select(DeviceTelemetry)
                .where(DeviceTelemetry.device_id == device.id)
                .order_by(desc(DeviceTelemetry.recorded_at))
                .limit(48)
            ).all()
            telemetry_series[device.device_id] = list(reversed(_serialize_telemetry(samples)))

    return templates.TemplateResponse(
        request,
        "dashboard.html",
        {
            "devices": devices,
            "telemetry_series": telemetry_series,
            "online_count": len([device for device in devices if device.display_status == "online"]),
            "page_title": "Fleet Overview",
            "format_temperature": _format_temperature,
            "format_timestamp": _format_timestamp,
        },
    )


@app.get("/devices/{device_id}", response_class=HTMLResponse)
def device_detail(request: Request, device_id: str):
    now = _utcnow()
    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.output_assignments), selectinload(Device.fermentation_config))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return HTMLResponse("Device not found", status_code=404)
        device = _apply_display_status(device, now)

        heartbeats = session.scalars(
            select(DeviceHeartbeat)
            .where(DeviceHeartbeat.device_id == device.id)
            .order_by(desc(DeviceHeartbeat.recorded_at))
            .limit(120)
        ).all()
        telemetry = session.scalars(
            select(DeviceTelemetry)
            .where(DeviceTelemetry.device_id == device.id)
            .order_by(desc(DeviceTelemetry.recorded_at))
            .limit(240)
        ).all()

        discovered_relays = session.scalars(
            select(DiscoveredRelay)
            .order_by(desc(DiscoveredRelay.last_seen_at), DiscoveredRelay.alias)
        ).all()

        assignments = {assignment.role: assignment for assignment in device.output_assignments}

    return templates.TemplateResponse(
        request,
        "device_detail.html",
        {
            "device": device,
            "fermentation_config": device.fermentation_config,
            "heartbeats": list(reversed(_serialize_heartbeats(heartbeats))),
            "telemetry": list(reversed(_serialize_telemetry(telemetry))),
            "discovered_relays": discovered_relays,
            "assignments": assignments,
            "heating_on_button_state": _output_button_state(device.heating_state, "on"),
            "heating_off_button_state": _output_button_state(device.heating_state, "off"),
            "cooling_on_button_state": _output_button_state(device.cooling_state, "on"),
            "cooling_off_button_state": _output_button_state(device.cooling_state, "off"),
            "page_title": f"{device.device_id} detail",
            "format_temperature": _format_temperature,
            "format_timestamp": _format_timestamp,
        },
    )


@app.get("/api/devices/{device_id}/live")
def device_live(device_id: str):
    now = _utcnow()
    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.fermentation_config))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)
        return JSONResponse(_device_live_payload(device, now))


@app.post("/devices/{device_id}/discover")
def discover_kasa(device_id: str):
    mqtt_bridge.publish_command(device_id, {"command": "discover_kasa"})
    return RedirectResponse(url=f"/devices/{device_id}", status_code=303)


@app.post("/devices/{device_id}/outputs")
async def update_outputs(request: Request, device_id: str):
    form = await request.form()
    heating_host = str(form.get("heating_host", "")).strip() or str(form.get("heating_host_manual", "")).strip()
    cooling_host = str(form.get("cooling_host", "")).strip() or str(form.get("cooling_host_manual", "")).strip()
    heating_alias_manual = str(form.get("heating_alias_manual", "")).strip()
    cooling_alias_manual = str(form.get("cooling_alias_manual", "")).strip()

    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.output_assignments))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return HTMLResponse("Device not found", status_code=404)

        heating_relay = None
        cooling_relay = None
        if heating_host:
            heating_relay = session.scalar(select(DiscoveredRelay).where(DiscoveredRelay.host == heating_host))
            if heating_relay is None:
                heating_relay = DiscoveredRelay(
                    source_device_id=device.id,
                    host=heating_host,
                    alias=heating_alias_manual or None,
                    driver="kasa_local",
                    port=9999,
                )
                session.add(heating_relay)
        if cooling_host:
            cooling_relay = session.scalar(select(DiscoveredRelay).where(DiscoveredRelay.host == cooling_host))
            if cooling_relay is None:
                cooling_relay = DiscoveredRelay(
                    source_device_id=device.id,
                    host=cooling_host,
                    alias=cooling_alias_manual or None,
                    driver="kasa_local",
                    port=9999,
                )
                session.add(cooling_relay)

        _upsert_assignment(session, device, "heating", heating_relay)
        _upsert_assignment(session, device, "cooling", cooling_relay)
        session.flush()

        mqtt_bridge.publish_system_config(device.device_id, _build_system_config_payload(device))
        session.commit()

    return RedirectResponse(url=f"/devices/{device_id}", status_code=303)


@app.post("/devices/{device_id}/fermentation")
async def update_fermentation(request: Request, device_id: str):
    form = await request.form()

    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.fermentation_config))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return HTMLResponse("Device not found", status_code=404)

        config = _get_or_create_fermentation_config(session, device)
        config.schema_version = 1
        config.desired_version = int(form.get("desired_version", config.desired_version or 1))
        config.name = str(form.get("name", "")).strip() or "Default fermentation"
        config.mode = str(form.get("mode", "thermostat")).strip() or "thermostat"
        config.setpoint_c = float(form.get("setpoint_c", config.setpoint_c))
        config.hysteresis_c = float(form.get("hysteresis_c", config.hysteresis_c))
        config.cooling_delay_s = int(form.get("cooling_delay_s", config.cooling_delay_s))
        config.heating_delay_s = int(form.get("heating_delay_s", config.heating_delay_s))
        config.primary_offset_c = float(form.get("primary_offset_c", config.primary_offset_c))
        config.secondary_enabled = form.get("secondary_enabled") == "on"
        config.control_sensor = str(form.get("control_sensor", "primary")).strip() or "primary"
        if config.secondary_enabled:
            config.secondary_offset_c = float(form.get("secondary_offset_c", config.secondary_offset_c or 0.0))
            config.secondary_limit_hysteresis_c = float(
                form.get("secondary_limit_hysteresis_c", config.secondary_limit_hysteresis_c or 1.5)
            )
        else:
            config.secondary_offset_c = None
            config.secondary_limit_hysteresis_c = None
            config.control_sensor = "primary"

        payload = _build_fermentation_config_payload(device, config)
        session.commit()
        mqtt_bridge.publish_fermentation_config(device.device_id, payload)

    return RedirectResponse(url=f"/devices/{device_id}", status_code=303)


@app.post("/devices/{device_id}/commands")
async def command_device(request: Request, device_id: str):
    form = await request.form()
    action = str(form.get("action", "")).strip()

    command_map = {
        "heating_on": {"command": "set_output", "target": "heating", "state": "on"},
        "heating_off": {"command": "set_output", "target": "heating", "state": "off"},
        "cooling_on": {"command": "set_output", "target": "cooling", "state": "on"},
        "cooling_off": {"command": "set_output", "target": "cooling", "state": "off"},
        "all_off": {"command": "set_output", "target": "all", "state": "off"},
    }

    payload = command_map.get(action)
    if payload is not None:
        mqtt_bridge.publish_command(device_id, payload)

    return RedirectResponse(url=f"/devices/{device_id}", status_code=303)


@app.get("/firmware/manifest/{channel}.json")
def firmware_manifest(request: Request, channel: str):
    if channel != settings.firmware_channel:
        raise HTTPException(status_code=404, detail="Firmware channel not found")

    firmware_path = _firmware_file_path()
    if not firmware_path.is_file():
        raise HTTPException(status_code=404, detail="Firmware binary not available")

    published_at = datetime.fromtimestamp(firmware_path.stat().st_mtime, tz=timezone.utc)
    return JSONResponse(
        {
            "version": settings.firmware_version,
            "channel": settings.firmware_channel,
            "published_at": published_at.isoformat(),
            "min_schema_version": settings.firmware_min_schema_version,
            "sha256": _firmware_sha256(firmware_path),
            "download_url": _firmware_download_url(request, firmware_path.name),
        }
    )


@app.get("/firmware/files/{filename}", name="firmware_file")
def firmware_file(filename: str):
    if Path(filename).name != filename:
        raise HTTPException(status_code=400, detail="Invalid firmware filename")

    file_path = _firmware_file_path(filename)
    if not file_path.is_file():
        raise HTTPException(status_code=404, detail="Firmware file not found")

    return FileResponse(file_path, media_type="application/octet-stream", filename=filename)
