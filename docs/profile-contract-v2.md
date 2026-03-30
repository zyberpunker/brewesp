# Profile contract v2 notes

The active v2 contract now lives in these two source files:

- [schemas/fermentation-config.schema.json](C:/Users/ola/git/brewesp/docs/schemas/fermentation-config.schema.json)
- [mqtt-contract.md](C:/Users/ola/git/brewesp/docs/mqtt-contract.md)

This file is intentionally no longer a proposal spec. It exists only to capture
the rollout boundary for the active contract:

- firmware, web publishing, and docs must move together
- invalid config must still be rejected without changing relay behavior
- runtime progress belongs in MQTT `state.profile_runtime`
- profile control commands belong on MQTT `command`

If the active contract changes again, update the schema and MQTT contract first,
then keep this note aligned with the rollout boundary.
