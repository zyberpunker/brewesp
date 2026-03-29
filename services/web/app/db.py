from __future__ import annotations

import logging
import time
from contextlib import contextmanager

from sqlalchemy import create_engine, text
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.orm import Session, declarative_base, sessionmaker

from .config import settings

logger = logging.getLogger(__name__)

engine = create_engine(settings.database_url, future=True, pool_pre_ping=True)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False, future=True)
Base = declarative_base()


def _apply_bootstrap_migrations() -> None:
    with engine.begin() as connection:
        devices_exists = connection.execute(
            text(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM information_schema.tables
                    WHERE table_schema = 'public'
                      AND table_name = 'devices'
                )
                """
            )
        ).scalar_one()
        if not devices_exists:
            return

        for statement in (
            "ALTER TABLE devices ADD COLUMN IF NOT EXISTS last_temp_c DOUBLE PRECISION",
            "ALTER TABLE devices ADD COLUMN IF NOT EXISTS last_secondary_temp_c DOUBLE PRECISION",
            "ALTER TABLE devices ADD COLUMN IF NOT EXISTS last_target_temp_c DOUBLE PRECISION",
            "ALTER TABLE devices ADD COLUMN IF NOT EXISTS last_mode VARCHAR(24)",
            "ALTER TABLE device_fermentation_configs ADD COLUMN IF NOT EXISTS primary_offset_c DOUBLE PRECISION DEFAULT 0.0",
            "ALTER TABLE device_fermentation_configs ADD COLUMN IF NOT EXISTS secondary_enabled BOOLEAN DEFAULT FALSE",
            "ALTER TABLE device_fermentation_configs ADD COLUMN IF NOT EXISTS secondary_offset_c DOUBLE PRECISION",
            "ALTER TABLE device_fermentation_configs ADD COLUMN IF NOT EXISTS secondary_limit_hysteresis_c DOUBLE PRECISION",
            "ALTER TABLE device_fermentation_configs ADD COLUMN IF NOT EXISTS control_sensor VARCHAR(24) DEFAULT 'primary'",
        ):
            connection.execute(text(statement))


def _prepare_hypertable(table_name: str) -> None:
    with engine.begin() as connection:
        connection.execute(text("CREATE EXTENSION IF NOT EXISTS timescaledb"))

        table_exists = connection.execute(
            text(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM information_schema.tables
                    WHERE table_schema = 'public'
                      AND table_name = :table_name
                )
                """
            ),
            {"table_name": table_name},
        ).scalar_one()
        if not table_exists:
            return

        is_hypertable = connection.execute(
            text(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM timescaledb_information.hypertables
                    WHERE hypertable_schema = 'public'
                      AND hypertable_name = :table_name
                )
                """
            ),
            {"table_name": table_name},
        ).scalar_one()
        if is_hypertable:
            return

        row_count = connection.execute(text(f"SELECT COUNT(*) FROM {table_name}")).scalar_one()
        if row_count:
            raise RuntimeError(
                f"{table_name} exists as a regular table with data and cannot be converted "
                "safely by the bootstrap migration. Back up the data and recreate the table manually."
            )

        logger.info("Recreating empty %s table to match TimescaleDB hypertable requirements", table_name)
        connection.execute(text(f"DROP TABLE {table_name}"))


def init_db(max_attempts: int = 30, retry_delay_s: float = 2.0) -> None:
    from . import models  # noqa: F401

    last_error: SQLAlchemyError | None = None
    for attempt in range(1, max_attempts + 1):
        try:
            _apply_bootstrap_migrations()
            _prepare_hypertable("device_heartbeats")
            _prepare_hypertable("device_telemetry")
            Base.metadata.create_all(bind=engine)
            with engine.begin() as connection:
                for table_name in ("device_heartbeats", "device_telemetry"):
                    connection.execute(
                        text(
                            f"""
                            SELECT create_hypertable(
                                '{table_name}',
                                'recorded_at',
                                if_not_exists => TRUE,
                                migrate_data => TRUE
                            )
                            """
                        )
                    )
            return
        except SQLAlchemyError as exc:
            last_error = exc
            logger.warning(
                "Database not ready during init attempt %s/%s: %s",
                attempt,
                max_attempts,
                exc,
            )
            if attempt == max_attempts:
                break
            time.sleep(retry_delay_s)

    assert last_error is not None
    raise last_error


@contextmanager
def session_scope() -> Session:
    session = SessionLocal()
    try:
        yield session
        session.commit()
    except Exception:
        session.rollback()
        raise
    finally:
        session.close()
