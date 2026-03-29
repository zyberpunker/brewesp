from __future__ import annotations

import os
from dataclasses import dataclass
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
    )


settings = _resolve_settings()
