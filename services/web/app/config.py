from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlparse


@dataclass(frozen=True)
class Settings:
    database_url: str = os.getenv(
        "BREWESP_DATABASE_URL",
        "postgresql+psycopg://brewesp:brewesp@localhost:5432/brewesp",
    )
    mqtt_url: str = os.getenv("BREWESP_MQTT_URL", "")
    mqtt_host: str = os.getenv("BREWESP_MQTT_HOST", "localhost")
    mqtt_port: int = int(os.getenv("BREWESP_MQTT_PORT", "1883"))
    mqtt_username: str = os.getenv("BREWESP_MQTT_USERNAME", "")
    mqtt_password: str = os.getenv("BREWESP_MQTT_PASSWORD", "")
    mqtt_topic_prefix: str = os.getenv("BREWESP_MQTT_TOPIC_PREFIX", "brewesp")
    app_title: str = "brewesp control"
    firmware_dir: Path = Path(os.getenv("BREWESP_FIRMWARE_DIR", "/app/firmware-files"))
    firmware_base_url: str = os.getenv("BREWESP_FIRMWARE_BASE_URL", "")
    firmware_channel: str = os.getenv("BREWESP_FIRMWARE_CHANNEL", "stable")
    firmware_filename: str = os.getenv("BREWESP_FIRMWARE_FILENAME", "firmware.bin")
    firmware_version: str = os.getenv("BREWESP_FIRMWARE_VERSION", "0.1.0-dev")
    firmware_min_schema_version: int = int(os.getenv("BREWESP_FIRMWARE_MIN_SCHEMA_VERSION", "1"))


def _resolve_settings() -> Settings:
    base = Settings()
    if not base.mqtt_url:
        return base

    parsed = urlparse(base.mqtt_url)
    return Settings(
        database_url=base.database_url,
        mqtt_url=base.mqtt_url,
        mqtt_host=parsed.hostname or base.mqtt_host,
        mqtt_port=parsed.port or base.mqtt_port,
        mqtt_username=parsed.username or base.mqtt_username,
        mqtt_password=parsed.password or base.mqtt_password,
        mqtt_topic_prefix=base.mqtt_topic_prefix,
        app_title=base.app_title,
        firmware_dir=base.firmware_dir,
        firmware_base_url=base.firmware_base_url,
        firmware_channel=base.firmware_channel,
        firmware_filename=base.firmware_filename,
        firmware_version=base.firmware_version,
        firmware_min_schema_version=base.firmware_min_schema_version,
    )


settings = _resolve_settings()
