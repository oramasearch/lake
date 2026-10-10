# Train → Streams

Streams follows Experiments in Train. Files are `frontend/src/pages/streams/StreamsPage.tsx`,
`TopicPage.tsx` and `common.tsx`.

`/w/<workspace_id>/train/streams` lists the workspace's exposed topics, grouped as *Workspace
Zetta* and *Lake Bus*, with subscriber count, held events, next sequence and a name-prefix filter.
The list defaults to 50 rows (1 to 200) and pages with **Previous** and **Next** cursors; the page
size, prefix and cursor are sent to `list_streams` and kept in the URL. Names are shown without
Zetta's `topic:` resource prefix, and agent topics are labelled (*Agent events*, *Agent tasks ·
<agent>*). Shared Agent tasks rows show unavailable counts rather than a misleading global total.

`/w/<workspace_id>/train/stream?customer_id=<space>&zetta=<workspace|bus>&topic=<name>` opens one
topic: normalized settings, subscription positions and lag, and held messages newest first. Each
message shows sequence, sender, publication time (Unix milliseconds), byte size, revision and
pretty-printed JSON; a payload over 1 KiB starts collapsed. **Copy message** copies the displayed
payload. **Older messages**, **Newest messages** and **Jump to sequence** browse by sequence, never
with a consuming receive. Both routes mark Streams current in the rail (`owns: ["/train/stream"]`).

**Live** long-polls `browse_stream` forward from the newest sequence: at most 100 events per read,
10-second waits, new messages on top, at most 200 kept on the page. Unchecking Live aborts the
poll; checking it resumes. Refresh shows later edits to older messages. The browser never contacts
Zetta or holds its service token. On the workspace's `agent-events` topic, **Turn** and **Agent**
filters (`task=`, `agent=`) are kept across jump, older/newest and Live reads. Other spaces'
payloads and shared subscription identities do not reach the page.

## Layout (lake#561)

The list opens with figures from `list_streams` (Topics, Held messages, Subscribers; topics whose
counts are unavailable are left out of the sums). Below them are the prefix and page-size filter,
and a card of rows per Zetta. **New topic** opens a drawer from the header.

The topic page is a message browser in a Split:

- The main card holds the messages as rows, with **Live** as a switch in its header. The switch
  starts *Paused*, and turning it off stops the poll. Jump and the agent-events filters sit above
  the rows.
- Editing a message and **Publish message** open dialogs. Edit Settings opens a drawer.
- The side column holds the topic's facts, its settings, its subscriptions with lag, and Purge and
  Delete Topic in a danger card.

## Controls and capabilities

| Control | Capability |
|---|---|
| Prefix filter, page size and cursor links | `list_streams` |
| Topic settings and subscriptions | `get_stream` |
| Messages, paging, jump, Live/pause | `browse_stream` (copy and pause are local) |
| New Topic with JSON settings | `create_stream` |
| Edit Settings, with the trim count it would cause | `update_stream` (a dry run first) |
| Delete Topic, by typed name | `delete_stream` |
| Publish (JSON) | `publish_stream_message` |
| Edit a payload at its displayed revision | `update_stream_message` |
| Delete a message, Purge before a sequence, with the count first | `delete_stream_messages` (a dry run first) |
| Which controls show | `my_permissions` on the space (`write`, `admin`) and the topic's `can_manage` |

New Topic accepts retention, limits and delivery defaults as validated settings JSON; new topics
live on the space's workspace Zetta. Edit Settings has only the mutable fields and asks for
confirmation with the count a retention shrink would trim. Changing a payload sends its revision;
a stale edit shows the server's refusal. The re-delivery checkbox starts unchecked and explains
that enabling it queues the replacement for subscribers which already received it. Deleting
messages shows the count and that pending subscribers will skip them, and sends that count as
`confirm_count`. Deleting a topic requires typing its name.

System topics are marked *system · browse-only* and have no write controls; lake refuses the same
writes to REST and MCP. Space readers see reads; writers see publish, edit and New Topic; admins
also see message deletion and purge. Only the topic's recorded creator (`can_manage`) sees
settings and Delete Topic, and the capability also requires admin to drop. `my_permissions` only
decides which controls show; every capability checks again. Ask Orama and ApplicationAgent may
use the read capabilities freely and must ask the person to confirm writes.

## Links in

App event panels link to `app-events.<app-id>`, and app builds and the Packages build log to
`build-events.<build-id>`. Run detail links to the workspace's Streams list: training does not
advertise a per-run topic, so lake does not invent one. ApplicationAgent, Ask Orama and Agent
Factory turns link to `/w/<workspace_id>/train/stream?zetta=workspace&topic=agent-events&task=<task-id>`
(*View this turn in Streams*). Viewing never advances a consumer.

## What changed (lake#561)

The server page's controls were `data-capability` forms driven by a page script; the app calls
the same capabilities itself, so the `data-capability` parity test no longer applies. The
workspace comes from the URL; the page passes `customer_id` and `workspace_id` explicitly to every
call.

## Verify and change

In a workspace: create a topic, publish, edit a payload (and a stale one), page older and newer,
jump, turn Live on and publish from another tab, preview and confirm a purge, edit settings with a
retention shrink, and delete the topic by name. Sign in as a reader and check that the write
controls are gone, and open a system topic. The API contract, examples and tests are in
[../api/streams.md](../api/streams.md).

A new control calls a registered capability; never add a page-only write.
