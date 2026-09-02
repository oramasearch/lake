# The HTTP server

**What.** lake serves HTTP/1 with Beast2 on the Capy + Corosio runtime, the stack Zetta serves
with. Port 4900 by default (`--listen HOST:PORT`).

**Where.** `include/lake/api/server.hpp` is the public shape (`ApiServer`, `ServerOptions`) and
names no HTTP type. `src/api/http_server.cpp` is the one translation unit that includes Beast2,
`boost::http` or Boost.JSON. Pages are pure renderers in `src/api/web_ui.cpp` (string in, string
out) and are the unit-tested surface.

**Lifecycle.** `ApiServer::start()`: build the router → construct
`beast2::http_server(io_context, workers, router, parser_cfg, serializer_cfg)` → `bind` →
`local_endpoint().port()` → `start()` → an `io_thread` runs `io_context.run()`. `stop()`:
`server->stop()`, `io_context.stop()`, join. The owner loop calls `wait_for_work()` then
`drain()` each tick; both are placeholders until lake has a node to marshal onto.

**Routes.**

| Route | Serves |
|---|---|
| `GET /` | the hello page (server-rendered, self-contained, no script) |
| `GET /style.css` | the stylesheet, `public, max-age=86400` |
| `GET /v1/server_info` | `{"name":"lake","node":…,"version":…,"api":"v1"}` |
| anything else | `404 {"error":"not_found"}` |

**Rules that keep it correct.**

- Every response goes through `send_text`, which stamps headers Zetta's way: `erase(etag)`,
  `erase(content_length)`, `set_payload_size(body)`. Beast2 frames from the declared size; never
  set those by hand.
- Routes are registered in order: `router.use(cors())`, the `router.add(...)` list, then a
  **trailing `router.use` 404 handler**. The router falls through (`route_next`) past the last
  route and nothing answers otherwise: without the trailing handler an unmatched request
  **hangs**. Keep it last.

**Known log noise.** Beast2's worker prints `read_header error: end of stream` to stderr for every
idle keep-alive connection a client closes (`modules/beast2/src/http_worker.cpp:64`, a raw
`std::cerr`, not the logger). In a pod that is one line per readiness probe. Not an error. The fix
is a private beast2 patch carrier (bridge issue #4).

**Verify.** `./build/lake --listen 127.0.0.1:4900 --node x` then `curl -i` the four cases above;
`ctest --test-dir build`.
