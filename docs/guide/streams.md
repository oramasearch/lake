# Streams

Streams shows the event topics of your workspace: the messages apps, builds, training and agents
publish, and topics you create for your own events. You browse what a topic holds without consuming
it, follow it live, publish and edit messages, and manage the topics you created.

Open **Train → Streams**. Every call below names `customer_id`, the id of the account that owns your
workspaces (yours, or your organization's), and `workspace_id`, the workspace in the page address
([Workspaces](/docs/workspaces)). Each operation is a REST call (`POST /v1/<name>` with a JSON body;
reads also answer `GET`) and an MCP tool of the same name and input.

## Concepts

| Term | What it means |
|---|---|
| **Topic** | A named, ordered log of messages kept for a while by its retention settings. |
| **Message** | One JSON event with a sequence number, a sender, a publication time, a size and a revision. |
| **Workspace** | The topics of this workspace: `agent-events`, `app-events.<app id>`, `build-events.<build id>`, training events, and the topics you create. Named `workspace` in calls. |
| **Lake Bus** | The dispatch topics that hand tasks to Lake's agents (`agent-tasks.<agent>`). Named `bus` in calls. You see only your own tasks there. |
| **Subscription** | A consumer of a topic, with its position and lag. Browsing never moves it. |
| **System topic** | A topic Lake or the engine owns. It is marked *system · browse-only* and can only be read. |
| **Creator** | The person who created a topic through Lake. Only the creator can change its settings or delete it. |

Sequence numbers are the topic's own, so gaps are normal.

## See the Topics

**Train → Streams** opens with figures (Topics, Held messages, Subscribers) and a card of topics per
place they live, *Workspace* and *Lake Bus*, each with its subscribers, held messages and next sequence.
Agent topics are labelled (*Agent events*, *Agent tasks · <agent>*). Filter by **Name prefix** and
page size, and page with **Previous** and **Next**.

- REST: `POST /v1/list_streams` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "prefix": "orders", "limit": 50}`
- MCP: `list_streams`. `limit` is 1 to 200 (default 50); pass `next_cursor` or `previous_cursor` back
  as `cursor` with the same prefix and limit.

## Create a Topic

1. On **Train → Streams** choose **New topic**.
2. Name it and, optionally, give its settings as JSON: retention (`retention_ms`, `max_events`),
   time to live, limits and delivery defaults. New topics live in this workspace.
3. Choose **Create topic**. You are recorded as its creator.

- REST: `POST /v1/create_stream` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "settings": {"retention_ms": 86400000, "max_events": 1000}}`
- MCP: `create_stream`. Lake makes the topic visible to browsing and keeps 1,000 messages unless you
  set `max_events`.

## Browse a Topic

Open a topic. **Topic Settings** shows its settings, **Subscribers** its subscriptions with their
positions and **Lag**, and the main card its messages, newest first. Each message shows its
sequence, sender, publication time, size, revision and pretty-printed JSON; a payload over 1 KiB
starts collapsed. **Copy message** copies it.

- **Older messages** and **Newest messages** page through the topic.
- **Jump** goes to a sequence number.
- On `agent-events`, the turn and agent boxes and **Filter events** show one agent turn. A link
  *View this turn in Streams* from Ask Orama, Apps or the Agent Factory opens exactly that.

Browsing never subscribes, acknowledges or moves any subscription.

- REST: `GET /v1/get_stream?customer_id=<customer id>&workspace_id=<workspace id>&zetta=workspace&topic=orders`
  reads the settings and subscriptions.
- REST: `POST /v1/browse_stream` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "from_sequence": 2, "direction": "forward", "limit": 100}`
- MCP: `get_stream` and `browse_stream`. The default is newest first (`backward`), 100 messages;
  `limit` is 1 to 1000. Use `next_page_sequence` to continue until it is empty, then
  `next_sequence` to follow new messages.

## Follow It Live

Turn on **Live** in the messages card (it starts *Paused*). The page waits for new messages,
up to 100 per read with 10-second waits, and puts each new one on top, keeping at most 200 on the
page. Turning it off stops waiting. Live reads leave subscriber positions unchanged.

- REST: `POST /v1/browse_stream` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "from_sequence": 3, "direction": "forward", "limit": 100, "wait_ms": 10000}`
- MCP: `browse_stream`. `wait_ms` is 0 to 30000.

## Publish a Message

On a topic you may write to, choose **Publish message**, enter the JSON payload and **Publish**.

- REST: `POST /v1/publish_stream_message` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "payload": "{\"status\": \"ready\"}"}`
- MCP: `publish_stream_message`. The payload is a JSON string, checked before it is sent. Read
  `accepted` in the answer: a topic's overflow policy can decline a valid message.

## Edit a Message

Choose **Edit** on a message, change the payload and **Save payload**. The edit is made at the
revision you saw; if someone changed the message since, it is refused and you refresh first. The
re-delivery box starts unticked; ticking it queues the edited message again for subscribers that
already received it. Subscribers that have not received it see the new payload either way. The
sequence and message id stay the same.

- REST: `POST /v1/update_stream_message` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "sequence": 2, "expected_revision": 1, "payload": "{\"status\": \"done\"}", "redeliver": false}`
- MCP: `update_stream_message`.

## Delete or Purge Messages

**Delete message** removes one message; under **Purge and Delete**, **Purge Messages** removes every
message before a sequence. Both show the count first (**Preview purge**), say that subscribers that
have not received these messages will skip them, and send that count with the deletion.

- Preview: `POST /v1/delete_stream_messages` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "before_sequence": 3, "dry_run": true}`
- Delete: the same with the previewed count,
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "before_sequence": 3, "confirm_count": 2}`
- MCP: `delete_stream_messages`. Give exactly one of `sequences` (1 to 1000 sequence numbers) or
  `before_sequence`. A count that no longer matches is refused with `conflict`.

## Change a Topic's Settings

As the topic's creator, choose **Edit** on **Topic Settings**, change the settings you may change,
and **Preview and save settings**. If the new retention would remove held messages, the page says
how many and asks before it saves.

- Preview: `POST /v1/update_stream` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "settings": {"max_events": 500}, "dry_run": true}`
- Save: the same without `dry_run`. The answer's `trimmed` is how many messages were removed.
- MCP: `update_stream`. Structural settings set at creation cannot be changed.

## Delete a Topic

As the topic's creator and an administrator, under **Purge and Delete** type the topic's name and
choose **Delete topic**. This removes all held messages and closes subscriptions.

- REST: `POST /v1/delete_stream` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "zetta": "workspace", "topic": "orders", "confirm_name": "orders"}`
- MCP: `delete_stream`.

## Permissions

| What | Needs |
|---|---|
| See topics, settings, subscriptions and messages | `read` on the account that owns the workspace |
| Create topics, publish and edit messages | `write` on that account |
| Delete messages and purge | `admin` on that account |
| Change a topic's settings | `write`, and being its creator |
| Delete a topic | `admin`, and being its creator |

The page shows only the controls you may use; every capability checks again. System topics refuse
every write, over REST and MCP as on the page. Ask Orama and ApplicationAgent read topics freely and
ask you to confirm any write.

## Limits

- System topics (everything on the Lake Bus, and the workspace's `agent-*`, `app-events.*`,
  `build-events.*`, `run-metrics.*` and `study-events.*` topics) are browse-only.
- A topic name must be concrete; wildcards are refused.
- Shared Agent tasks rows show their counts as unavailable rather than a total across accounts.
- The topic list shows only topics exposed for browsing.

## Troubleshooting

| What you see | What to do |
|---|---|
| No write controls on a topic | It is a system topic, or you have read access only. |
| No **Edit** on settings, or no **Delete topic** | Only the topic's creator sees them. |
| `conflict` on an edit | The message changed since you saw it; refresh and edit again. |
| `conflict` on a purge | The count changed; preview again. |
| `invalid_input` at `/workspace_id` | The workspace is not one of the account's live workspaces. |
| `workspace_not_ready` | The workspace is starting ([Workspaces](/docs/workspaces)). |
| A card shows an error | That place could not be read; the other card still lists its topics. |

## Related Pages

- [Apps](/docs/apps)
- [Packages and the Registry](/docs/registry)
- [Experiments](/docs/experiments)
- [Agents](/docs/agents)
- [Ask Orama](/docs/ask-orama)
