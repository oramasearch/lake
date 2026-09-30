# Telemetry implementation

`metrics.cpp` instruments and forwards Store operations. Stream ownership and customer-scoped
agent-task enumeration and prior-conversation lookup preserve the wrapped store's results and
errors; the metrics layer adds no authorization or topic state. See
`docs/control-plane/streams.md` for those contracts.

The same wrapper forwards protected-app resolution, session authorization, code creation,
exchange, and cleanup without changing their results. Forward-auth timing remains visible through
the HTTP `Server-Timing` regression described in `docs/serving/http.md`.
