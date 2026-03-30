from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy import JSON, Boolean, DateTime, Float, ForeignKey, Integer, String, Text
from sqlalchemy.orm import Mapped, mapped_column, relationship

from .db import Base


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class Device(Base):
    __tablename__ = "devices"

    id: Mapped[int] = mapped_column(primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    status: Mapped[str] = mapped_column(String(24), default="unknown")
    fw_version: Mapped[str | None] = mapped_column(String(64), nullable=True)
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_heartbeat_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_state_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    mqtt_connected: Mapped[bool] = mapped_column(Boolean, default=False)
    ui_mode: Mapped[str | None] = mapped_column(String(24), nullable=True)
    heating_state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    cooling_state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    last_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    last_secondary_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    last_target_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    last_mode: Mapped[str | None] = mapped_column(String(24), nullable=True)
    last_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_heap_free: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_payload: Mapped[dict | None] = mapped_column(JSON, nullable=True)
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)

    heartbeats: Mapped[list["DeviceHeartbeat"]] = relationship(back_populates="device")
    telemetry_samples: Mapped[list["DeviceTelemetry"]] = relationship(back_populates="device")
    fermentation_config: Mapped["DeviceFermentationConfig | None"] = relationship(
        back_populates="device",
        uselist=False,
    )
    output_assignments: Mapped[list["DeviceOutputAssignment"]] = relationship(back_populates="device")
    discovered_relays: Mapped[list["DiscoveredRelay"]] = relationship(back_populates="source_device")


class DeviceHeartbeat(Base):
    __tablename__ = "device_heartbeats"

    device_id: Mapped[int] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    recorded_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, primary_key=True)
    uptime_s: Mapped[int | None] = mapped_column(Integer, nullable=True)
    wifi_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    heap_free: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ui_mode: Mapped[str | None] = mapped_column(String(24), nullable=True)
    heating_state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    cooling_state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    payload: Mapped[dict] = mapped_column(JSON)

    device: Mapped[Device] = relationship(back_populates="heartbeats")


class DeviceTelemetry(Base):
    __tablename__ = "device_telemetry"

    device_id: Mapped[int] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    recorded_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, primary_key=True)
    temp_primary_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    temp_secondary_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    setpoint_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    effective_target_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    mode: Mapped[str | None] = mapped_column(String(24), nullable=True)
    profile_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    profile_step_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    heating_active: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    cooling_active: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    payload: Mapped[dict] = mapped_column(JSON)

    device: Mapped[Device] = relationship(back_populates="telemetry_samples")


class DeviceFermentationConfig(Base):
    __tablename__ = "device_fermentation_configs"

    device_id: Mapped[int] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    schema_version: Mapped[int] = mapped_column(Integer, default=2)
    desired_version: Mapped[int] = mapped_column(Integer, default=1)
    name: Mapped[str | None] = mapped_column(String(128), nullable=True)
    mode: Mapped[str] = mapped_column(String(24), default="thermostat")
    setpoint_c: Mapped[float] = mapped_column(Float, default=20.0)
    hysteresis_c: Mapped[float] = mapped_column(Float, default=0.3)
    cooling_delay_s: Mapped[int] = mapped_column(Integer, default=300)
    heating_delay_s: Mapped[int] = mapped_column(Integer, default=120)
    primary_offset_c: Mapped[float] = mapped_column(Float, default=0.0)
    secondary_enabled: Mapped[bool] = mapped_column(Boolean, default=False)
    secondary_offset_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    secondary_limit_hysteresis_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    control_sensor: Mapped[str] = mapped_column(String(24), default="primary")
    deviation_c: Mapped[float] = mapped_column(Float, default=2.0)
    sensor_stale_s: Mapped[int] = mapped_column(Integer, default=30)
    profile_plan: Mapped[dict | None] = mapped_column(JSON, nullable=True)
    last_applied_version: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_applied_result: Mapped[str | None] = mapped_column(String(24), nullable=True)
    last_applied_message: Mapped[str | None] = mapped_column(Text, nullable=True)
    last_applied_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)

    device: Mapped[Device] = relationship(back_populates="fermentation_config")


class DeviceOutputAssignment(Base):
    __tablename__ = "device_output_assignments"

    device_id: Mapped[int] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    role: Mapped[str] = mapped_column(String(24), primary_key=True)
    driver: Mapped[str] = mapped_column(String(32), default="none")
    host: Mapped[str | None] = mapped_column(String(128), nullable=True)
    port: Mapped[int] = mapped_column(Integer, default=9999)
    alias: Mapped[str | None] = mapped_column(String(128), nullable=True)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)

    device: Mapped[Device] = relationship(back_populates="output_assignments")


class DiscoveredRelay(Base):
    __tablename__ = "discovered_relays"

    id: Mapped[int] = mapped_column(primary_key=True)
    source_device_id: Mapped[int | None] = mapped_column(ForeignKey("devices.id"), nullable=True, index=True)
    driver: Mapped[str] = mapped_column(String(32), default="kasa_local")
    host: Mapped[str] = mapped_column(String(128), unique=True, index=True)
    port: Mapped[int] = mapped_column(Integer, default=9999)
    alias: Mapped[str | None] = mapped_column(String(128), nullable=True)
    model: Mapped[str | None] = mapped_column(String(128), nullable=True)
    is_on: Mapped[bool | None] = mapped_column(Boolean, nullable=True)
    last_seen_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)
    raw_payload: Mapped[dict | None] = mapped_column(JSON, nullable=True)

    source_device: Mapped[Device | None] = relationship(back_populates="discovered_relays")
