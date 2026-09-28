# Telemetry implementation

`metrics.cpp` instruments and forwards Store operations. Stream ownership and customer-scoped
agent-task enumeration preserve the wrapped store's results and errors; the metrics layer adds
no authorization or topic state. See `docs/control-plane/streams.md` for those contracts.
