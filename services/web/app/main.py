from __future__ import annotations

from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from hashlib import sha256
from pathlib import Path
import re
from xml.etree import ElementTree as ET

from fastapi import FastAPI, File, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from sqlalchemy import desc, select
from sqlalchemy.orm import selectinload

from .config import settings
from .db import SessionLocal, init_db
from .models import (
    Device,
    DeviceFermentationConfig,
    DeviceOutputAssignment,
    DeviceTelemetry,
    DiscoveredRelay,
    FermentationProfile,
)
from .mqtt_bridge import MqttBridge

templates = Jinja2Templates(directory="app/templates")
mqtt_bridge = MqttBridge()


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    _seed_builtin_profiles()
    mqtt_bridge.start()
    yield
    mqtt_bridge.stop()


app = FastAPI(title=settings.app_title, lifespan=lifespan)
app.mount("/static", StaticFiles(directory="app/static"), name="static")

STALE_AFTER_SECONDS = 90
OFFLINE_AFTER_SECONDS = 180
MAX_PROFILE_STEPS = 10
MAX_LIBRARY_PROFILE_STEPS = 7
MAX_PROFILE_TARGET_C = 50.0
MIN_PROFILE_TARGET_C = -20.0
MAX_PROFILE_DURATION_S = 3596400
DEFAULT_DEVICE_CHART_HOURS = 12
MAX_DEVICE_TELEMETRY_SAMPLES = 20000


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


def _serialize_telemetry(samples: list[DeviceTelemetry]) -> list[dict]:
    return [
        {
            "recorded_at": sample.recorded_at.isoformat(),
            "temp_primary_c": sample.temp_primary_c,
            "temp_secondary_c": sample.temp_secondary_c,
            "setpoint_c": sample.setpoint_c,
            "effective_target_c": sample.effective_target_c,
            "mode": sample.mode,
            "profile_id": sample.profile_id,
            "profile_step_id": sample.profile_step_id,
            "heating_active": sample.heating_active,
            "cooling_active": sample.cooling_active,
        }
        for sample in samples
    ]


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _slugify(value: str, fallback: str = "profile") -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", value.strip().lower()).strip("-")
    return slug or fallback


def _profile_step_duration(days: float) -> int:
    return int(round(days * 86400))


def _build_profile_step(
    step_id: str,
    label: str,
    target_c: float,
    hold_duration_s: int,
    *,
    advance_policy: str = "auto",
    ramp_mode: str | None = None,
    ramp_duration_s: int | None = None,
) -> dict:
    step = {
        "id": step_id,
        "label": label,
        "target_c": target_c,
        "hold_duration_s": hold_duration_s,
        "advance_policy": advance_policy,
    }
    if ramp_mode == "time" and ramp_duration_s and ramp_duration_s > 0:
        step["ramp"] = {
            "mode": "time",
            "duration_s": ramp_duration_s,
        }
    return step


def _build_builtin_profile_payload(slug: str) -> dict:
    if slug == "ale":
        return {
            "schema_version": 1,
            "steps": [
                _build_profile_step("step-1", "Step 1", 19.0, _profile_step_duration(7)),
                _build_profile_step("step-2", "Step 2", 21.0, _profile_step_duration(3)),
            ],
        }
    if slug == "lager":
        return {
            "schema_version": 1,
            "steps": [
                _build_profile_step("step-1", "Step 1", 11.0, _profile_step_duration(10)),
                _build_profile_step("step-2", "Step 2", 15.0, _profile_step_duration(3)),
            ],
        }
    raise ValueError(f"Unsupported builtin profile slug: {slug}")


def _seed_builtin_profiles() -> None:
    builtin_profiles = (
        ("ale", "Ale"),
        ("lager", "Lager"),
    )
    with SessionLocal() as session:
        changed = False
        for slug, name in builtin_profiles:
            profile = session.scalar(
                select(FermentationProfile).where(FermentationProfile.slug == slug)
            )
            if profile is not None:
                continue
            session.add(
                FermentationProfile(
                    slug=slug,
                    name=name,
                    source="builtin",
                    is_builtin=True,
                    profile_data=_build_builtin_profile_payload(slug),
                )
            )
            changed = True
        if changed:
            session.commit()


def _ensure_unique_profile_slug(session, name: str, *, preferred_slug: str | None = None) -> str:
    base_slug = preferred_slug or _slugify(name)
    slug = base_slug
    suffix = 2
    while session.scalar(select(FermentationProfile).where(FermentationProfile.slug == slug)) is not None:
        slug = f"{base_slug}-{suffix}"
        suffix += 1
    return slug


def _load_device_telemetry(
    session,
    device_pk: int,
    *,
    hours: int | None = None,
    limit: int | None = None,
) -> list[DeviceTelemetry]:
    stmt = select(DeviceTelemetry).where(DeviceTelemetry.device_id == device_pk)

    if hours is not None:
        stmt = stmt.where(DeviceTelemetry.recorded_at >= (_utcnow() - timedelta(hours=hours)))

    sample_limit = limit if limit is not None else MAX_DEVICE_TELEMETRY_SAMPLES
    stmt = stmt.order_by(desc(DeviceTelemetry.recorded_at)).limit(
        max(1, min(sample_limit, MAX_DEVICE_TELEMETRY_SAMPLES))
    )
    return session.scalars(stmt).all()


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


def _sensor_status_label(present: bool | None, valid: bool | None, stale: bool | None) -> str:
    if not present:
        return "missing"
    if stale:
        return "stale"
    if valid:
        return "present"
    return "invalid"


def _device_live_payload(device: Device, now: datetime) -> dict:
    device = _apply_display_status(device, now)
    fermentation_config = device.fermentation_config
    state_payload = device.last_payload or {}
    beer_probe_present = state_payload.get("beer_probe_present")
    beer_probe_valid = state_payload.get("beer_probe_valid")
    beer_probe_stale = state_payload.get("beer_probe_stale")
    chamber_probe_present = state_payload.get("chamber_probe_present")
    chamber_probe_valid = state_payload.get("chamber_probe_valid")
    chamber_probe_stale = state_payload.get("chamber_probe_stale")
    fault = state_payload.get("fault")
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
        "last_rssi": device.last_rssi,
        "last_heap_free": device.last_heap_free,
        "heating_on_button_state": _output_button_state(device.heating_state, "on"),
        "heating_off_button_state": _output_button_state(device.heating_state, "off"),
        "cooling_on_button_state": _output_button_state(device.cooling_state, "on"),
        "cooling_off_button_state": _output_button_state(device.cooling_state, "off"),
        "last_mode": device.last_mode,
        "controller_state": state_payload.get("controller_state"),
        "controller_reason": state_payload.get("controller_reason"),
        "fault": fault,
        "has_fault": bool(fault),
        "automatic_control_active": state_payload.get("automatic_control_active"),
        "active_config_version": state_payload.get("active_config_version"),
        "beer_probe_present": beer_probe_present,
        "beer_probe_valid": beer_probe_valid,
        "beer_probe_stale": beer_probe_stale,
        "beer_probe_status": _sensor_status_label(beer_probe_present, beer_probe_valid, beer_probe_stale),
        "beer_probe_rom": state_payload.get("beer_probe_rom"),
        "chamber_probe_present": chamber_probe_present,
        "chamber_probe_valid": chamber_probe_valid,
        "chamber_probe_stale": chamber_probe_stale,
        "chamber_probe_status": _sensor_status_label(
            chamber_probe_present, chamber_probe_valid, chamber_probe_stale
        ),
        "chamber_probe_rom": state_payload.get("chamber_probe_rom"),
        "secondary_sensor_enabled": state_payload.get("secondary_sensor_enabled"),
        "control_sensor": state_payload.get("control_sensor"),
        "profile_runtime": state_payload.get("profile_runtime"),
        "last_payload": state_payload,
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


def _serialize_discovered_relay(relay: DiscoveredRelay) -> dict:
    return {
        "host": relay.host,
        "alias": relay.alias,
        "driver": relay.driver,
        "port": relay.port,
        "model": relay.model,
        "is_on": relay.is_on,
        "last_seen_at": relay.last_seen_at.isoformat() if relay.last_seen_at else None,
        "last_seen_label": _format_timestamp(relay.last_seen_at),
    }


def _build_output_routing_payload(device: Device, discovered_relays: list[DiscoveredRelay]) -> dict:
    payload = _build_system_config_payload(device)
    payload["discovered_relays"] = [_serialize_discovered_relay(relay) for relay in discovered_relays]
    return payload


def _default_profile_plan(config: DeviceFermentationConfig) -> dict:
    return {
        "id": f"{(config.name or 'fermentation').strip().lower().replace(' ', '-') or 'fermentation'}-plan",
        "steps": [
            {
                "id": "step-1",
                "label": config.name or "Current target",
                "target_c": config.setpoint_c,
                "hold_duration_s": 0,
                "advance_policy": "manual_release",
            }
        ],
    }


def _build_fermentation_config_payload(device: Device, config: DeviceFermentationConfig) -> dict:
    payload = {
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
            "deviation_c": config.deviation_c,
            "sensor_stale_s": config.sensor_stale_s,
        },
    }
    if config.mode == "profile":
        payload["profile"] = config.profile_plan or _default_profile_plan(config)
    return payload


def _store_profile_payload(config: DeviceFermentationConfig, payload: dict) -> None:
    config.schema_version = int(payload.get("schema_version", 2))
    config.desired_version = int(payload.get("version", config.desired_version or 1))
    config.name = str(payload.get("name", "")).strip() or "Default fermentation"
    config.mode = str(payload.get("mode", "thermostat")).strip() or "thermostat"

    thermostat = payload.get("thermostat") or {}
    sensors = payload.get("sensors") or {}
    alarms = payload.get("alarms") or {}

    config.setpoint_c = float(thermostat.get("setpoint_c", config.setpoint_c))
    config.hysteresis_c = float(thermostat.get("hysteresis_c", config.hysteresis_c))
    config.cooling_delay_s = int(thermostat.get("cooling_delay_s", config.cooling_delay_s))
    config.heating_delay_s = int(thermostat.get("heating_delay_s", config.heating_delay_s))
    config.primary_offset_c = float(sensors.get("primary_offset_c", config.primary_offset_c))
    config.secondary_enabled = bool(sensors.get("secondary_enabled", config.secondary_enabled))
    config.control_sensor = str(sensors.get("control_sensor", config.control_sensor)).strip() or "primary"
    config.deviation_c = float(alarms.get("deviation_c", config.deviation_c))
    config.sensor_stale_s = int(alarms.get("sensor_stale_s", config.sensor_stale_s))

    if config.secondary_enabled:
        config.secondary_offset_c = float(sensors.get("secondary_offset_c", config.secondary_offset_c or 0.0))
        config.secondary_limit_hysteresis_c = float(
            sensors.get("secondary_limit_hysteresis_c", config.secondary_limit_hysteresis_c or 1.5)
        )
    else:
        config.secondary_offset_c = None
        config.secondary_limit_hysteresis_c = None
        config.control_sensor = "primary"

    if config.mode == "profile":
        profile = payload.get("profile")
        config.profile_plan = profile if isinstance(profile, dict) else _default_profile_plan(config)
    else:
        config.profile_plan = None


def _build_profile_step_id(label: str, index: int, used_ids: set[str]) -> str:
    base_id = _slugify(label, fallback=f"step-{index + 1}")
    candidate = base_id
    suffix = 2
    while candidate in used_ids:
        candidate = f"{base_id}-{suffix}"
        suffix += 1
    used_ids.add(candidate)
    return candidate


def _validate_profile_library_steps(steps_payload: object) -> list[dict]:
    if not isinstance(steps_payload, list) or not steps_payload:
        raise ValueError("Profile must include at least one step.")
    if len(steps_payload) > MAX_LIBRARY_PROFILE_STEPS:
        raise ValueError(f"Profile must include 1-{MAX_LIBRARY_PROFILE_STEPS} steps.")

    normalized_steps: list[dict] = []
    used_ids: set[str] = set()
    for index, raw_step in enumerate(steps_payload):
        if not isinstance(raw_step, dict):
            raise ValueError(f"steps[{index}] must be an object.")

        label = str(raw_step.get("label", "")).strip() or f"Step {index + 1}"
        try:
            target_c = float(raw_step["target_c"])
        except (KeyError, TypeError, ValueError):
            raise ValueError(f"steps[{index}].target_c must be numeric.") from None
        if target_c < MIN_PROFILE_TARGET_C or target_c > MAX_PROFILE_TARGET_C:
            raise ValueError(
                f"steps[{index}].target_c must be between {MIN_PROFILE_TARGET_C} and {MAX_PROFILE_TARGET_C}."
            )

        try:
            hold_duration_s = int(raw_step["hold_duration_s"])
        except (KeyError, TypeError, ValueError):
            raise ValueError(f"steps[{index}].hold_duration_s must be an integer.") from None
        if hold_duration_s < 0 or hold_duration_s > MAX_PROFILE_DURATION_S:
            raise ValueError(
                f"steps[{index}].hold_duration_s must be between 0 and {MAX_PROFILE_DURATION_S}."
            )

        advance_policy = str(raw_step.get("advance_policy", "auto")).strip() or "auto"
        if advance_policy not in {"auto", "manual_release"}:
            raise ValueError(f"steps[{index}].advance_policy must be auto or manual_release.")

        ramp = raw_step.get("ramp")
        normalized_step = {
            "id": _build_profile_step_id(label, index, used_ids),
            "label": label,
            "target_c": target_c,
            "hold_duration_s": hold_duration_s,
            "advance_policy": advance_policy,
        }
        if ramp is not None:
            if not isinstance(ramp, dict):
                raise ValueError(f"steps[{index}].ramp must be an object.")
            ramp_mode = str(ramp.get("mode", "")).strip()
            if ramp_mode != "time":
                raise ValueError(f"steps[{index}].ramp.mode must be time.")
            try:
                ramp_duration_s = int(ramp["duration_s"])
            except (KeyError, TypeError, ValueError):
                raise ValueError(f"steps[{index}].ramp.duration_s must be an integer.") from None
            if ramp_duration_s < 0 or ramp_duration_s > MAX_PROFILE_DURATION_S:
                raise ValueError(
                    f"steps[{index}].ramp.duration_s must be between 0 and {MAX_PROFILE_DURATION_S}."
                )
            if ramp_duration_s > 0:
                normalized_step["ramp"] = {
                    "mode": "time",
                    "duration_s": ramp_duration_s,
                }
        normalized_steps.append(normalized_step)
    return normalized_steps


def _build_profile_library_payload(name: str, steps_payload: object) -> dict:
    return {
        "schema_version": 1,
        "steps": _validate_profile_library_steps(steps_payload),
    }


def _serialize_profile_summary(profile: FermentationProfile) -> dict:
    steps = profile.profile_data.get("steps", [])
    total_duration_s = 0
    for step in steps:
        total_duration_s += int(step.get("hold_duration_s", 0))
        ramp = step.get("ramp")
        if isinstance(ramp, dict) and ramp.get("mode") == "time":
            total_duration_s += int(ramp.get("duration_s", 0))
    return {
        "slug": profile.slug,
        "name": profile.name,
        "source": profile.source,
        "is_builtin": profile.is_builtin,
        "imported_from": profile.imported_from,
        "step_count": len(steps),
        "total_duration_s": total_duration_s,
        "updated_at": profile.updated_at.isoformat() if profile.updated_at else None,
    }


def _build_device_profile_from_library(profile: FermentationProfile) -> dict:
    steps = []
    for step in profile.profile_data.get("steps", []):
        entry = {
            "id": step["id"],
            "label": step["label"],
            "target_c": step["target_c"],
            "hold_duration_s": step["hold_duration_s"],
            "advance_policy": step["advance_policy"],
        }
        ramp = step.get("ramp")
        if isinstance(ramp, dict) and ramp.get("mode") == "time" and int(ramp.get("duration_s", 0)) > 0:
            entry["ramp_duration_s"] = int(ramp["duration_s"])
        steps.append(entry)
    return {
        "id": profile.slug,
        "steps": steps,
    }


def _serialize_profile_detail(profile: FermentationProfile) -> dict:
    payload = _serialize_profile_summary(profile)
    payload["steps"] = profile.profile_data.get("steps", [])
    payload["schema_version"] = int(profile.profile_data.get("schema_version", 1))
    payload["device_profile"] = _build_device_profile_from_library(profile)
    return payload


def _read_xml_text(raw_bytes: bytes) -> bytes:
    if raw_bytes.startswith(b"\xef\xbb\xbf"):
        return raw_bytes[3:]
    return raw_bytes


def _parse_beerxml_profile(raw_bytes: bytes, filename: str | None = None) -> tuple[str, dict]:
    try:
        root = ET.fromstring(_read_xml_text(raw_bytes))
    except ET.ParseError as exc:
        raise ValueError(f"BeerXML parse error: {exc}") from None

    recipe = root.find("RECIPE")
    if recipe is None:
        raise ValueError("BeerXML file must include a RECIPE node.")

    def _text(field: str) -> str:
        value = recipe.findtext(field)
        return value.strip() if isinstance(value, str) else ""

    profile_name = _text("BF_FERMENTATION_PROFILE_NAME") or _text("NAME") or "Imported profile"
    stage_specs = (
        ("PRIMARY", "Primary"),
        ("SECONDARY", "Secondary"),
        ("TERTIARY", "Tertiary"),
        ("AGE", "Ageing"),
    )
    imported_steps = []
    for prefix, label in stage_specs:
        age_text = _text(f"{prefix}_AGE")
        temp_text = _text(f"{prefix}_TEMP")
        if not age_text and not temp_text:
            continue
        if not age_text or not temp_text:
            raise ValueError(f"BeerXML {prefix.lower()} stage must include both age and temperature.")
        try:
            age_days = float(age_text)
            target_c = float(temp_text)
        except ValueError:
            raise ValueError(f"BeerXML {prefix.lower()} stage contains invalid numeric values.") from None
        imported_steps.append(
            _build_profile_step(
                step_id=f"step-{len(imported_steps) + 1}",
                label=label,
                target_c=target_c,
                hold_duration_s=_profile_step_duration(age_days),
                advance_policy="auto",
            )
        )

    if not imported_steps:
        raise ValueError("BeerXML file did not contain supported fermentation stages.")
    if len(imported_steps) > MAX_LIBRARY_PROFILE_STEPS:
        raise ValueError(
            f"BeerXML profile contains {len(imported_steps)} stages, but the web profile manager supports at most {MAX_LIBRARY_PROFILE_STEPS}."
        )

    return profile_name, {
        "schema_version": 1,
        "steps": _validate_profile_library_steps(imported_steps),
        "source_filename": filename,
    }


def _build_profile_command_payload(command: str, step_id: str | None = None) -> dict:
    payload: dict = {"command": command, "requested_by": "web", "ts": _utcnow().isoformat()}
    if step_id:
        payload["args"] = {"step_id": step_id}
    return payload


def _validate_v2_payload(payload: dict) -> None:
    def _require_float(section: dict, field: str, *, error_field: str | None = None) -> float:
        try:
            return float(section[field])
        except (KeyError, TypeError, ValueError):
            raise ValueError(f"{error_field or field} must be numeric") from None

    def _require_int(section: dict, field: str, *, error_field: str | None = None) -> int:
        try:
            return int(section[field])
        except (KeyError, TypeError, ValueError):
            raise ValueError(f"{error_field or field} must be an integer") from None

    if payload.get("schema_version", 2) != 2:
        raise ValueError("schema_version must be 2")
    if payload.get("mode") not in {"thermostat", "profile"}:
        raise ValueError("mode must be thermostat or profile")
    thermostat = payload.get("thermostat")
    sensors = payload.get("sensors")
    alarms = payload.get("alarms")
    if not isinstance(thermostat, dict):
        raise ValueError("thermostat payload is required")
    if not isinstance(sensors, dict):
        raise ValueError("sensors payload is required")
    if not isinstance(alarms, dict):
        raise ValueError("alarms payload is required")
    try:
        thermostat_setpoint_c = float(thermostat["setpoint_c"])
    except (KeyError, TypeError, ValueError):
        raise ValueError("thermostat.setpoint_c must be numeric") from None
    if thermostat_setpoint_c < MIN_PROFILE_TARGET_C or thermostat_setpoint_c > MAX_PROFILE_TARGET_C:
        raise ValueError(
            f"thermostat.setpoint_c must be between {MIN_PROFILE_TARGET_C} and {MAX_PROFILE_TARGET_C}"
        )
    thermostat_hysteresis_c = _require_float(
        thermostat, "hysteresis_c", error_field="thermostat.hysteresis_c"
    )
    if thermostat_hysteresis_c < 0.1 or thermostat_hysteresis_c > 5.0:
        raise ValueError("thermostat.hysteresis_c must be between 0.1 and 5.0")
    cooling_delay_s = _require_int(
        thermostat, "cooling_delay_s", error_field="thermostat.cooling_delay_s"
    )
    if cooling_delay_s < 0 or cooling_delay_s > 3600:
        raise ValueError("thermostat.cooling_delay_s must be between 0 and 3600")
    heating_delay_s = _require_int(
        thermostat, "heating_delay_s", error_field="thermostat.heating_delay_s"
    )
    if heating_delay_s < 0 or heating_delay_s > 3600:
        raise ValueError("thermostat.heating_delay_s must be between 0 and 3600")

    primary_offset_c = _require_float(
        sensors, "primary_offset_c", error_field="sensors.primary_offset_c"
    )
    if primary_offset_c < -5.0 or primary_offset_c > 5.0:
        raise ValueError("sensors.primary_offset_c must be between -5.0 and 5.0")
    secondary_enabled = sensors.get("secondary_enabled")
    if not isinstance(secondary_enabled, bool):
        raise ValueError("sensors.secondary_enabled must be true or false")
    control_sensor = str(sensors.get("control_sensor", "")).strip()
    if control_sensor not in {"primary", "secondary"}:
        raise ValueError("sensors.control_sensor must be primary or secondary")
    if not secondary_enabled and control_sensor != "primary":
        raise ValueError("sensors.control_sensor must be primary when secondary sensor is disabled")

    deviation_c = _require_float(alarms, "deviation_c", error_field="alarms.deviation_c")
    if deviation_c < 0.0 or deviation_c > 25.0:
        raise ValueError("alarms.deviation_c must be between 0.0 and 25.0")
    sensor_stale_s = _require_int(
        alarms, "sensor_stale_s", error_field="alarms.sensor_stale_s"
    )
    if sensor_stale_s < 1 or sensor_stale_s > 600:
        raise ValueError("alarms.sensor_stale_s must be between 1 and 600")

    if secondary_enabled:
        secondary_offset_c = _require_float(
            sensors, "secondary_offset_c", error_field="sensors.secondary_offset_c"
        )
        if secondary_offset_c < -5.0 or secondary_offset_c > 5.0:
            raise ValueError("sensors.secondary_offset_c must be between -5.0 and 5.0")
        secondary_limit_hysteresis_c = _require_float(
            sensors,
            "secondary_limit_hysteresis_c",
            error_field="sensors.secondary_limit_hysteresis_c",
        )
        if secondary_limit_hysteresis_c < 0.1 or secondary_limit_hysteresis_c > 25.0:
            raise ValueError(
                "sensors.secondary_limit_hysteresis_c must be between 0.1 and 25.0"
            )

    if payload.get("mode") == "profile":
        profile = payload.get("profile")
        if not isinstance(profile, dict):
            raise ValueError("profile payload is required for profile mode")
        profile_id = str(profile.get("id", "")).strip()
        if not profile_id:
            raise ValueError("profile.id is required for profile mode")
        steps = profile.get("steps")
        if not isinstance(steps, list) or not steps:
            raise ValueError("profile must include at least one step")
        if len(steps) > MAX_PROFILE_STEPS:
            raise ValueError(f"profile must include 1-{MAX_PROFILE_STEPS} steps")
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                raise ValueError(f"profile.steps[{index}] must be an object")
            step_id = str(step.get("id", "")).strip()
            if not step_id:
                raise ValueError(f"profile.steps[{index}].id is required")
            try:
                target_c = float(step["target_c"])
            except (KeyError, TypeError, ValueError):
                raise ValueError(f"profile.steps[{index}].target_c must be numeric") from None
            if target_c < MIN_PROFILE_TARGET_C or target_c > MAX_PROFILE_TARGET_C:
                raise ValueError(
                    f"profile.steps[{index}].target_c must be between {MIN_PROFILE_TARGET_C} and {MAX_PROFILE_TARGET_C}"
                )
            try:
                hold_duration_s = int(step["hold_duration_s"])
            except (KeyError, TypeError, ValueError):
                raise ValueError(f"profile.steps[{index}].hold_duration_s must be an integer") from None
            if hold_duration_s < 0 or hold_duration_s > MAX_PROFILE_DURATION_S:
                raise ValueError(
                    f"profile.steps[{index}].hold_duration_s must be between 0 and {MAX_PROFILE_DURATION_S}"
                )
            if "ramp_duration_s" in step:
                try:
                    ramp_duration_s = int(step["ramp_duration_s"])
                except (TypeError, ValueError):
                    raise ValueError(f"profile.steps[{index}].ramp_duration_s must be an integer") from None
                if ramp_duration_s < 0 or ramp_duration_s > MAX_PROFILE_DURATION_S:
                    raise ValueError(
                        f"profile.steps[{index}].ramp_duration_s must be between 0 and {MAX_PROFILE_DURATION_S}"
                    )
            advance_policy = str(step.get("advance_policy", "")).strip()
            if advance_policy not in {"auto", "manual_release"}:
                raise ValueError(
                    f"profile.steps[{index}].advance_policy must be auto or manual_release"
                )


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


def _default_port_for_driver(driver: str) -> int:
    if driver == "shelly_http_rpc":
        return 80
    return 9999


def _resolve_routing_relay(
    session,
    device: Device,
    *,
    host: str,
    driver: str,
    alias: str = "",
) -> DiscoveredRelay | None:
    normalized_host = host.strip()
    normalized_driver = driver.strip()
    normalized_alias = alias.strip() or None
    if not normalized_host:
        return None
    if normalized_driver not in {"kasa_local", "shelly_http_rpc"}:
        normalized_driver = "kasa_local"

    relay = session.scalar(select(DiscoveredRelay).where(DiscoveredRelay.host == normalized_host))
    if relay is None:
        relay = DiscoveredRelay(
            source_device_id=device.id,
            host=normalized_host,
            alias=normalized_alias,
            driver=normalized_driver,
            port=_default_port_for_driver(normalized_driver),
        )
        session.add(relay)
        return relay

    relay.source_device_id = relay.source_device_id or device.id
    previous_driver = relay.driver
    relay.driver = normalized_driver
    if previous_driver != normalized_driver or not relay.port:
        relay.port = _default_port_for_driver(normalized_driver)
    if normalized_alias:
        relay.alias = normalized_alias
    return relay


def _get_or_create_fermentation_config(session, device: Device) -> DeviceFermentationConfig:
    config = device.fermentation_config
    if config is None:
        config = DeviceFermentationConfig(
            device_id=device.id,
            schema_version=2,
            desired_version=1,
            name="Default fermentation",
            mode="thermostat",
            setpoint_c=20.0,
            hysteresis_c=0.3,
            cooling_delay_s=300,
            heating_delay_s=120,
            primary_offset_c=0.0,
            secondary_enabled=False,
            control_sensor="primary",
            deviation_c=2.0,
            sensor_stale_s=30,
        )
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


OUTPUT_COMMAND_MAP = {
    "heating_on": {"command": "set_output", "target": "heating", "state": "on"},
    "heating_off": {"command": "set_output", "target": "heating", "state": "off"},
    "cooling_on": {"command": "set_output", "target": "cooling", "state": "on"},
    "cooling_off": {"command": "set_output", "target": "cooling", "state": "off"},
    "all_off": {"command": "set_output", "target": "all", "state": "off"},
}


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


@app.get("/profiles", response_class=HTMLResponse)
def profiles_page(request: Request):
    return templates.TemplateResponse(
        request,
        "profiles.html",
        {
            "page_title": "Fermentation Profiles",
        },
    )


@app.get("/api/fermentation-profiles")
def list_fermentation_profiles():
    with SessionLocal() as session:
        profiles = session.scalars(
            select(FermentationProfile).order_by(
                desc(FermentationProfile.is_builtin),
                FermentationProfile.name,
            )
        ).all()
        return JSONResponse([_serialize_profile_summary(profile) for profile in profiles])


@app.get("/api/fermentation-profiles/{profile_slug}")
def fermentation_profile_detail(profile_slug: str):
    with SessionLocal() as session:
        profile = session.scalar(
            select(FermentationProfile).where(FermentationProfile.slug == profile_slug)
        )
        if profile is None:
            return JSONResponse({"error": "Profile not found"}, status_code=404)
        return JSONResponse(_serialize_profile_detail(profile))


@app.post("/api/fermentation-profiles")
async def create_fermentation_profile(request: Request):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    name = str(payload.get("name", "")).strip()
    if not name:
        return JSONResponse({"error": "Profile name is required"}, status_code=400)

    try:
        profile_data = _build_profile_library_payload(name, payload.get("steps"))
    except ValueError as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)

    with SessionLocal() as session:
        profile = FermentationProfile(
            slug=_ensure_unique_profile_slug(session, name),
            name=name,
            source="manual",
            is_builtin=False,
            profile_data=profile_data,
        )
        session.add(profile)
        session.commit()
        session.refresh(profile)
        return JSONResponse(_serialize_profile_detail(profile), status_code=201)


@app.put("/api/fermentation-profiles/{profile_slug}")
async def update_fermentation_profile(request: Request, profile_slug: str):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    name = str(payload.get("name", "")).strip()
    if not name:
        return JSONResponse({"error": "Profile name is required"}, status_code=400)

    try:
        profile_data = _build_profile_library_payload(name, payload.get("steps"))
    except ValueError as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)

    with SessionLocal() as session:
        profile = session.scalar(
            select(FermentationProfile).where(FermentationProfile.slug == profile_slug)
        )
        if profile is None:
            return JSONResponse({"error": "Profile not found"}, status_code=404)

        profile.name = name
        profile.profile_data = profile_data
        session.commit()
        session.refresh(profile)
        return JSONResponse(_serialize_profile_detail(profile))


@app.delete("/api/fermentation-profiles/{profile_slug}")
def delete_fermentation_profile(profile_slug: str):
    with SessionLocal() as session:
        profile = session.scalar(
            select(FermentationProfile).where(FermentationProfile.slug == profile_slug)
        )
        if profile is None:
            return JSONResponse({"error": "Profile not found"}, status_code=404)
        if profile.is_builtin:
            return JSONResponse({"error": "Built-in profiles cannot be deleted"}, status_code=400)
        session.delete(profile)
        session.commit()
        return JSONResponse({"status": "deleted"})


@app.post("/api/fermentation-profiles/import/beerxml")
async def import_fermentation_profile_beerxml(file: UploadFile = File(...)):
    raw_bytes = await file.read()
    if not raw_bytes:
        return JSONResponse({"error": "Uploaded file is empty"}, status_code=400)

    try:
        name, parsed_payload = _parse_beerxml_profile(raw_bytes, file.filename)
    except ValueError as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)

    with SessionLocal() as session:
        profile = FermentationProfile(
            slug=_ensure_unique_profile_slug(session, name),
            name=name,
            source="beerxml",
            is_builtin=False,
            imported_from=file.filename or None,
            profile_data={
                "schema_version": parsed_payload["schema_version"],
                "steps": parsed_payload["steps"],
            },
        )
        session.add(profile)
        session.commit()
        session.refresh(profile)
        return JSONResponse(_serialize_profile_detail(profile), status_code=201)


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

        telemetry = session.scalars(
            select(DeviceTelemetry)
            .where(DeviceTelemetry.device_id == device.id)
            .order_by(desc(DeviceTelemetry.recorded_at))
            .limit(240)
        ).all()

        live_payload = _device_live_payload(device, now)

    return templates.TemplateResponse(
        request,
        "device_detail.html",
        {
            "device_id": device.device_id,
            "bootstrap": {
                "deviceId": device.device_id,
                "initialLivePayload": live_payload,
                "initialTelemetry": list(reversed(_serialize_telemetry(telemetry))),
            },
            "page_title": f"{device.device_id} detail",
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


@app.get("/api/devices/{device_id}/output-routing")
def output_routing(device_id: str):
    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.output_assignments))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)

        discovered_relays = session.scalars(
            select(DiscoveredRelay)
            .order_by(desc(DiscoveredRelay.last_seen_at), DiscoveredRelay.alias)
        ).all()
        return JSONResponse(_build_output_routing_payload(device, discovered_relays))


@app.post("/api/devices/{device_id}/output-routing")
async def update_output_routing(request: Request, device_id: str):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    heating = payload.get("heating")
    cooling = payload.get("cooling")
    if not isinstance(heating, dict) or not isinstance(cooling, dict):
        return JSONResponse({"error": "heating and cooling payloads are required"}, status_code=400)

    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.output_assignments))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)

        heating_relay = _resolve_routing_relay(
            session,
            device,
            host=str(heating.get("host", "")),
            driver=str(heating.get("driver", "")),
            alias=str(heating.get("alias", "")),
        )
        cooling_relay = _resolve_routing_relay(
            session,
            device,
            host=str(cooling.get("host", "")),
            driver=str(cooling.get("driver", "")),
            alias=str(cooling.get("alias", "")),
        )

        _upsert_assignment(session, device, "heating", heating_relay)
        _upsert_assignment(session, device, "cooling", cooling_relay)
        session.flush()

        discovered_relays = session.scalars(
            select(DiscoveredRelay)
            .order_by(desc(DiscoveredRelay.last_seen_at), DiscoveredRelay.alias)
        ).all()
        publish_payload = _build_system_config_payload(device)
        response_payload = _build_output_routing_payload(device, discovered_relays)
        session.commit()

    mqtt_bridge.publish_system_config(device.device_id, publish_payload)
    return JSONResponse(response_payload)


@app.post("/api/devices/{device_id}/output-routing/discover")
def discover_output_relays(device_id: str):
    mqtt_bridge.publish_command(device_id, {"command": "discover_kasa"})
    return JSONResponse({"status": "queued", "command": "discover_kasa"})


@app.get("/api/devices/{device_id}/telemetry")
def device_telemetry(device_id: str, limit: int = 240, hours: int | None = None, all: bool = False):
    if hours is not None and (hours < 1 or hours > 24 * 30):
        return JSONResponse({"error": "hours must be between 1 and 720"}, status_code=400)

    with SessionLocal() as session:
        device = session.scalar(select(Device).where(Device.device_id == device_id))
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)

        telemetry_hours = None if all else hours
        samples = _load_device_telemetry(
            session,
            device.id,
            hours=telemetry_hours,
            limit=limit if telemetry_hours is None else MAX_DEVICE_TELEMETRY_SAMPLES,
        )
        return JSONResponse(list(reversed(_serialize_telemetry(samples))))


@app.post("/api/devices/{device_id}/commands/output")
async def output_command(request: Request, device_id: str):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    action = str(payload.get("action", "")).strip()
    command = OUTPUT_COMMAND_MAP.get(action)
    if command is None:
        return JSONResponse({"error": "Unsupported output action"}, status_code=400)

    mqtt_bridge.publish_command(device_id, command)
    return JSONResponse(command)


@app.get("/api/devices/{device_id}/fermentation-plan")
def fermentation_plan(device_id: str):
    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.fermentation_config))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)

        config = _get_or_create_fermentation_config(session, device)
        return JSONResponse(_build_fermentation_config_payload(device, config))


@app.post("/api/devices/{device_id}/fermentation-plan")
async def update_fermentation_plan(request: Request, device_id: str):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    try:
        _validate_v2_payload(payload)
    except ValueError as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)

    with SessionLocal() as session:
        device = session.scalar(
            select(Device)
            .options(selectinload(Device.fermentation_config))
            .where(Device.device_id == device_id)
        )
        if device is None:
            return JSONResponse({"error": "Device not found"}, status_code=404)

        config = _get_or_create_fermentation_config(session, device)
        _store_profile_payload(config, payload)
        publish_payload = _build_fermentation_config_payload(device, config)
        session.commit()

    mqtt_bridge.publish_fermentation_config(device_id, publish_payload)
    return JSONResponse(publish_payload)


@app.post("/api/devices/{device_id}/commands/profile")
async def profile_command(request: Request, device_id: str):
    try:
        payload = await request.json()
    except ValueError:
        return JSONResponse({"error": "Invalid JSON body"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"error": "Expected JSON object"}, status_code=400)

    command = str(payload.get("command", "")).strip()
    if command not in {
        "profile_pause",
        "profile_resume",
        "profile_release_hold",
        "profile_jump_to_step",
        "profile_stop",
    }:
        return JSONResponse({"error": "Unsupported profile command"}, status_code=400)

    step_id = str(payload.get("step_id", "")).strip() or None
    if command == "profile_jump_to_step" and step_id is None:
        return JSONResponse({"error": "step_id is required for profile_jump_to_step"}, status_code=400)
    mqtt_payload = _build_profile_command_payload(command, step_id)
    mqtt_bridge.publish_command(device_id, mqtt_payload)
    return JSONResponse(mqtt_payload)


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
