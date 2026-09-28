# Train → Streams

Streams follows Runs in Train. `/train/streams` lists the selected space's exposed topics grouped
by workspace Zetta and Lake bus Zetta, with subscriber count, held events, next sequence and a
name-prefix filter. `/train/stream?customer_id=<space>&zetta=<workspace|bus>&topic=<name>` opens
one topic. Both routes belong to Streams in `navigation_routes` and select its link with
`aria-current="page"`. No charts or extra stylesheet are used.

The topic page shows normalized settings, subscription positions and lag, and held messages
newest first. Each message shows sequence, sender, publication time (Unix milliseconds), byte
size, revision and pretty-printed JSON. Payloads over 1 KiB start collapsed. Copy uses the displayed
payload. Older/Newest links and Jump use sequence-based browsing, never a consuming receive.

Live uses `browse_stream` through Lake with forward sequence pages, a 100-event bound and a
10-second long poll. Uncheck Live to abort/pause; enabling it resumes from the last cursor.
The page retains up to 200 live message cards. Refresh shows later edits to old messages.
The browser never contacts Zetta or holds its service token. Agent-turn views filter the shared
bus on the server; other customers' payloads and subscription identities do not reach the page.

## Controls and capabilities

| Control | Registered capability |
|---|---|
| Space selection, prefix filter, grouped list | `list_streams` |
| Topic link, settings and subscriptions | `get_stream` |
| Messages, paging, jump, Live/pause, copy | `browse_stream` (copy/pause are local presentation) |
| New Topic with JSON settings | `create_stream` |
| Edit Settings and retention preview | `update_stream` |
| Typed-name Delete Topic | `delete_stream` |
| JSON Publish box | `publish_stream_message` |
| Edit Payload with displayed revision | `update_stream_message` |
| Delete Message, Purge Before Sequence, count preview | `delete_stream_messages` |

Each control or enclosing form declares `data-capability`; the parity test checks the registry
mapping. Forms submit JSON to the same public POST capabilities used by MCP. The HTTP page
adapter only selects the space, calls read capabilities and renders their values. It does not
provide a private mutation endpoint. Writes require JavaScript; read navigation remains ordinary
GET links/forms.

New Topic accepts retention, limits and delivery defaults as validated settings JSON. Edit
Settings contains only the mutable fields, and previews the count a retention shrink would trim.
Changing the displayed payload sends its revision; a stale edit shows the server's refusal.
The re-delivery checkbox starts unchecked and explains that enabling it queues the replacement
for subscribers which already received/acknowledged the message. Deleting messages previews a
count and explains that pending subscribers will skip them; confirmation sends that count.
Deleting a topic requires typing its name.

System topics are visibly marked browse-only and have no write controls. Lake enforces the same
refusal for direct REST/MCP calls. Space readers see reads; writers see publish/edit/create;
admins additionally see message deletion. Only the recorded creator sees settings/drop controls,
and drop also needs admin. Ask Orama and ApplicationAgent may use read capabilities freely and
must ask the person to confirm writes.

App event panels link directly to their `app-events.<app-id>` stream. App build actions and the
package build-log panel link to `build-events.<build-id>`. Run detail links to that space's
workspace Streams list: training does not currently advertise a canonical per-run pub/sub topic,
so Lake does not invent one. A deploy's app consumer and this page independently see all retained
events; viewing never advances that consumer.

Tests cover HTML escaping, hidden service credentials, both selected-nav routes, all control
mappings, the live long-poll contract and continued app-consumer delivery. API details and all
operation examples are in [streams.md](../api/streams.md).
