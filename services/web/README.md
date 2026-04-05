# Web service

Planned home for the Dockerized companion web service.

Responsibilities:

- settings UI
- profile editor
- reusable fermentation profile library
- BeerXML profile import
- device dashboard
- telemetry/history storage
- publish config JSON to MQTT
- subscribe to state/telemetry topics
- allow removing stale devices from the web registry when they are gone from MQTT

Recommended first stack:

- FastAPI
- Jinja templates
- TimescaleDB
- Mosquitto as MQTT broker

Current direction:

- server-rendered FastAPI app with a modern dashboard UI
- MQTT ingest for `availability`, `heartbeat`, and `state`
- time-series storage in TimescaleDB for heartbeat and later telemetry history
- manual device deletion from the dashboard only clears web-side stored state; a device that publishes MQTT again is recreated automatically

Run locally with Docker Compose from:

- [compose.yaml](C:\Users\ola\git\brewesp\infra\compose.yaml)
- copy [`.env.example`](C:\Users\ola\git\brewesp\infra\.env.example) to `infra/.env`

Expected local URLs/ports:

- web UI: `http://localhost:8000`
- MQTT broker: `localhost:1883`
- TimescaleDB/PostgreSQL: `localhost:5432`

Environment configuration:

- `BREWESP_DATABASE_URL`
- `BREWESP_MQTT_URL`
- `BREWESP_MQTT_HOST`
- `BREWESP_MQTT_PORT`
- `BREWESP_MQTT_USERNAME`
- `BREWESP_MQTT_PASSWORD`
- `BREWESP_MQTT_TOPIC_PREFIX`

External MQTT usage:

- if you already have an MQTT broker, set the broker values in `infra/.env`
- the bundled Mosquitto service is optional and only starts when you enable the
  Docker Compose profile `local-broker`
- otherwise run only `db` and `web`
