# Streams capabilities

Train → Streams exposes workspace topics and the space's Lake bus topics through the same
registry used by REST, MCP, Ask Orama and the ApplicationAgent. Reads may be offered freely;
agents must obtain the person's confirmation before writes. Browser credentials never contain a
Zetta token. Lake calls Zetta with the workspace or bus service credential; Zetta audits that
service identity, while Lake's signed audit records the person, space, topic, operation and outcome.
Payloads are not copied into Lake's audit details.

Every operation below accepts `POST /v1/<name>` with JSON and is an MCP tool of exactly the same
name and schema. Read operations also retain the registry's GET mapping. OpenAPI is generated at
`/v1/openapi.json`; MCP discovery is `tools/list`. MCP `tools/call` returns the same envelope in
`result.structuredContent`. Successful envelopes are
`{"ok":true,"value":<value>,"duration_ms":0}`; examples below show request bodies and their `value`.
Failures have `ok:false` and `error:{code,message,path}`. Schemas close every object, including
settings, subscriptions, events and headers. Arbitrary message JSON travels as a validated string
in `payload`, not as an unspecified schema object.

## Authorization and routing

| Lake capability / MCP tool | Space permission | Zetta capability |
|---|---|---|
| `list_streams` | read | `list_topics` |
| `get_stream` | read | `get_topic` |
| `browse_stream` | read | `browse_topic` |
| `create_stream` | write | `create_topic` |
| `update_stream` | write, topic creator | `update_topic` |
| `delete_stream` | admin, topic creator | `drop_topic` |
| `publish_stream_message` | write | `publish_event` |
| `update_stream_message` | write | `update_event` |
| `delete_stream_messages` | admin | `delete_events` |

`customer_id` is the space ID. `zetta` is `workspace` or `bus`; it is not a URL or service ID.
`list_streams` declares candidate authorization and explicitly checks the space's read permission
inside its handler before either Zetta is called. The other operations use registry authorization.
`topic` accepts a concrete name with an optional `topic:` prefix. Wildcards are refused. A
workspace request always resolves that customer's ready workspace. Bus requests resolve app,
build or agent-task rows and check their customer before calling Zetta; guessing another space's
identifier returns `not_found`. The global `agent-events` and agent work queues cannot be read.

The list contains only exposed topics. Bus topics are `app-events.<app-id>`,
`build-events.<build-id>` and virtual `agent-turn.<task-id>` views. A turn view filters the retained
global agent bus by `payload.task_id` on the server, hides shared subscriptions and counts only
its own held messages. Sequence numbers remain those of the source topic, so gaps are normal.
The allocator high-water mark is not a count of a turn's messages. Up to 32 source pages are
scanned per browse call; continue with `next_page_sequence`, even when the filtered page is empty.

All bus topics and reserved workspace `agent-*`, `app-events.*`, `build-events.*` and inbox names
are system, browse-only. Every write refuses them, including message writes. Other existing
workspace topics can be read and their messages changed with space permission, but only a
creator recorded by Lake can reconfigure or drop them. Ownership is durable and tied to the
workspace service incarnation; see [control-plane storage](../control-plane/streams.md).
Creation refuses an existing ownership record, even if the topic was dropped outside Lake.
Raw topic writes are no longer in `workspace_act`'s allowlist.

## Topic operations

`list_streams` takes an optional unqualified name `prefix`. Each Zetta has its own group and error
string, so an unavailable workspace does not hide accessible bus topics.

```json
{"customer_id":"space-id","prefix":"orders"}
```
```json
{"groups":[{"zetta":"workspace","topics":[{"topic":"topic:orders","subscribers":1,"held_events":2,"next_sequence":3,"system":false}],"error":""},{"zetta":"bus","topics":[],"error":""}]}
```

`create_stream` creates only on the workspace. `settings` is optional; Lake forces `exposed:true`
and supplies `max_events:1000` unless explicitly configured. All other defaults belong to Zetta.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","settings":{"retention_ms":86400000,"max_events":1000,"delivery":{"guarantee":"at_least_once"}}}
```

The response has `topic`, `exposed` and normalized `settings`, for example:

```json
{"topic":"topic:orders","exposed":true,"settings":{"retention_ms":86400000,"max_events":1000,"delivery":{"guarantee":"at_least_once"}}}
```

`get_stream` returns settings and subscription positions/lag without advancing them. The example's
settings are shown in reduced form; actual responses include Zetta's normalized settings fields.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders"}
```
```json
{"topic":"topic:orders","zetta":"workspace","system":false,"can_manage":true,"settings":{"retention_ms":86400000,"max_events":1000},"subscriber_count":1,"subscriptions":[{"id":1,"subscriber":"agent:consumer","positions":[1],"next_sequence":1,"lag":2,"in_flight":0,"redeliveries":0,"connected":true,"group":""}],"held_events":2,"earliest_sequence":1,"next_sequence":3}
```

`update_stream` accepts a settings merge patch. `dry_run:true` counts the currently held messages
that the resulting retention settings would remove, without changing Zetta. The UI displays this
count before applying the same patch without `dry_run`. Publication and expiration can change
the count between preview and application. The real response's `trimmed` is Zetta's actual count.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","settings":{"max_events":1},"dry_run":true}
```
```json
{"topic":"topic:orders","settings":{"retention_ms":86400000,"max_events":1000},"trimmed":1,"dry_run":true}
```

Create settings include retention (`retention_ms`, `max_events`), TTL, memory/disk limits,
overflow and over-bound policy, dispatch, priority aging, chain limit, delivery defaults and
delivery bounds. Optional overrides accept `null`; it clears the override. Delivery exposes
recipients/count, ordering/priority, guarantee, acknowledgment deadline and reply behavior.
Bounds expose maximum priority/recipients/guarantee and allowed recipient modes. Creation also
accepts lifecycle, store, ring capacity, inline threshold, priority levels and producer queue.
Those structural fields and `exposed` are absent from the update schema. Zetta additionally
enforces its runtime mutability constraints. Exact ranges/enums are in generated OpenAPI/MCP.

`delete_stream` requires both space admin and creator ownership, plus the exact typed name.
It drops all held messages and closes subscriptions.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","confirm_name":"orders"}
```
```json
{"topic":"topic:orders","exposed":true}
```

## Message operations

`browse_stream` never subscribes, acknowledges or advances a subscription. Defaults are newest
first (`direction:"backward"`), `limit:100`, `wait_ms:0`. `from_sequence` is inclusive; `limit` is
1–1000 and `wait_ms` is 0–30000. Live clients use forward reads from their last allocator mark.
Both REST and MCP support the bounded long poll. `next_sequence` is the allocator high-water mark,
not a page cursor: use nullable `next_page_sequence` until it is null, then `next_sequence` to tail.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","from_sequence":2,"direction":"forward","limit":100,"wait_ms":10000}
```
```json
{"topic":"topic:orders","events":[{"sequence":2,"id":"event-2","from":"service:lake","published_at":1790000000000,"payload":"{\"status\":\"ready\"}","size":18,"encoding":"json","headers":[],"revision":1,"edited_at":null,"edited_by":""}],"earliest_sequence":1,"next_sequence":3,"next_page_sequence":null}
```

`publish_stream_message` validates JSON before sending `publish_event` with `encoding:"json"`.
Check `accepted`; an overflow policy may decline a valid publication.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","payload":"{\"status\":\"ready\"}"}
```
```json
{"event_id":"event-2","accepted":true,"clamped":[]}
```

`update_stream_message` sends the displayed revision as `expected_revision`. A stale revision
returns `conflict`; refresh before editing again. `redeliver` defaults to false. Opting in queues
the edited payload again for subscribers that already received it, including acknowledged
deliveries. Pending subscribers see the replacement regardless. Sequence and event ID are stable.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","sequence":2,"expected_revision":1,"payload":"{\"status\":\"done\"}","redeliver":false}
```
```json
{"topic":"topic:orders","sequence":2,"revision":2,"edited_at":1790000001000,"edited_by":"service:lake"}
```

`delete_stream_messages` takes exactly one selector: unique `sequences` (1–1000 positive integers)
or exclusive `before_sequence`. Preview first with `dry_run:true`, explain that pending
subscribers skip the removed messages, then send the returned count as `confirm_count` without
`dry_run`. Lake re-counts and refuses a stale count with `conflict`. Concurrent expiry/publication
after that check may still affect the actual removal; the response reports the actual count.

```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","before_sequence":3,"dry_run":true}
```
```json
{"topic":"topic:orders","removed":2,"dry_run":true}
```
```json
{"customer_id":"space-id","zetta":"workspace","topic":"orders","before_sequence":3,"confirm_count":2}
```
```json
{"topic":"topic:orders","removed":2}
```

## Verification

`Streams.*` covers the two Zettas, server-side space isolation, system refusal, ownership and
permissions, non-consuming browse, stale revisions, paging and retention, REST/MCP schema parity,
one call per capability on each transport, navigation selection and UI capability mappings.
It also checks that generated agent documentation includes the Streams group and confirmation
guidance, and that a refused list makes no upstream workspace call.
`StreamsPostgres.*` checks durable ownership across reopen and workspace/space boundaries.
`SchemaConformance.*` independently validates successful stream outputs through the real server.
The Zetta CRUD runtime is required in deployed Zettas (Zetta PR #1143); Lake builds only its
existing transport client and tests use fake workspace/bus callers, not that runtime checkout.

### Native proof (2026-09-28)

Both builds use the authorized Zetta transport checkout through `LAKE_ZETTA_SOURCE_DIR` and the
Liquid bridge checkout through `LAKE_LIQUID_SOURCE_DIR`. The shared configuration sets
`BUILD_SHARED_LIBS=ON`; `otool -L` confirms its Liquid/Boost/PQXX dynamic libraries. Configure and
supply the PostgreSQL environment as described in [local.md](../build/local.md). This proof used
fresh local bases `lake_issue301_static_main_20260928` and `lake_issue301_shared_main_20260928`, with
process-owned fixtures under the worktree's `.cache/tmp`. Builds used at most six jobs; suites
ran one at a time. Both active build trees were configured from scratch after rebasing onto
`origin/main` at `75219cb` and updating submodules; no compiled objects from before that rebase
were reused.

```text
cmake --build build -j 6
cmake --build build-shared -j 6
ctest --test-dir build --output-on-failure -j 1
100% tests passed, 0 tests failed out of 409
ctest --test-dir build-shared --output-on-failure -j 1
100% tests passed, 0 tests failed out of 409
tools/ui/test-token-gates.sh
sync-design-system: ok — 8 files match 38959d9966aa
build-tokens: ok — src/api/web_ui_tokens.inc matches the vendored tokens
token-gates: ok — generated output and snapshot changes were rejected
```

Each suite retains 14 existing skips (browser-to-terminal, optional Hugging Face client and
Postgres workspace-UI variants) and one disabled search benchmark. No Streams test is skipped.
The focused `Streams.*` regression run passed all eight cases, including the explicit-grant
ownership case and every-control capability mapping. Schema, navigation and MCP checks also
pass in both full suites. The controls' JavaScript parses successfully, and the native binary's
help command runs.

Linux proof deferred: the supplied Linux-lane helper is absent. Interactive browser QA is
deferred because the in-app browser connection rejects missing sandbox metadata. A deployed
app-progress exercise requires the Zetta CRUD image rollout; fake bus tests verify independent
consumer delivery while browsing. The operator schema handoff is a separate rollout change.
