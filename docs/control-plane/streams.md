# Stream ownership and bus isolation

Topic creators live in the workspace Zetta that holds the topic (lake#379). Each creator is
one sealed record in that workspace's `lake_stream_owners` collection
([workspace records](../architecture/workspace-records.md)), keyed by the blind index of the
topic name. The sealed body names the space, the topic and the creator, and the seal binds the
workspace, so ownership follows that workspace incarnation: deleting or recreating the workspace
takes its topics and their creators together. Nothing about a topic's creator is kept in lake's
PostgreSQL. Migration `0037_streams` created `stream_owners`; migration
`0047_stream_owner_workspaces` records each of its rows against the workspace it names, and the
operator's `copy_workspace_records` moves them. The table is dropped by a later migration once the
copy is verified.

**Why the record store and not a topic property.** A topic's settings would be the obvious place,
but they are the wrong one. Zetta's topics are node-local and best effort, while a record is
replicated and its write is linearizable per key, so `if_absent` decides a race between two
creators atomically. Settings are also readable and writable through the same `update_topic` that a
creator may call, and they are returned to readers, whereas a record is sealed, and its key is a
blind index, so the workspace holds neither the topic name nor the creator in clear. Finally,
ownership must outlive an out-of-band `drop_topic`: the rule below refuses to recreate a name whose
creator is still recorded, and a property would vanish with the topic it guards.

Only a successful Zetta `create_topic` establishes the recorded creator: lake then writes the
record with `if_absent`. A failed or ambiguous creation never grants management of an existing
topic. Successful topic drop removes the record, conditionally on the revision lake read.
If Zetta succeeds but the following ownership write fails, the operation reports failure;
the topic remains readable but cannot be reconfigured/dropped until ownership is reconciled.
Lake does not infer creator ownership from a service credential or a topic's name.
An existing ownership record refuses creation before contacting Zetta, including after an
out-of-band topic drop or failed ownership cleanup. A second person cannot recreate that name
under the first person's permissions; reconcile the leftover record before reusing the name.
The regression test grants the second principal space write access explicitly, then verifies
the ownership conflict is returned without an upstream call or recreated topic.

Reading a creator costs one `get_record` on the workspace (`get_stream`, and before each write);
`list_streams` reads no ownership. Every ownership call goes through the workspace caller, so a
missing, malformed or unknown `workspace_id` is `invalid_input` at `/workspace_id`, a workspace of
another space is refused, and a workspace that is not ready refuses rather than falling back to
another workspace, the lake Zetta or PostgreSQL.

`list_agent_task_stream_scopes(customer_id)`
loads only task ID, agent and completion state in one store query so Streams can build the allowed
sets without a query per topic or loading task bodies. Those sets authorize and filter one
`agent-events` view and the `agent-tasks.<agent>` views used by that space. App/build topic
authorization likewise resolves durable rows and their customer IDs. Shared topics and
subscriptions are never exposed as space-owned resources.

Space policy supports `write` grants for stream publication and editing. Existing read/admin/owner
inheritance remains; deletion requires admin and topic drop/configuration additionally require
the exact creator. The registry records successful, failed and denied writes in the signed Lake
audit. Zetta's CRUD handlers independently audit the Lake service credential. Preview actions
are also audited but do not mutate Zetta.

Apply the schema before deploying this binary. Lake pods do not run migrations. The operator
must vendor `db/schema.sql` and advance its guard to `app_forward_auth_guard` in its own rollout
change; this worktree does not modify the operator repository. An existing installation needs the
owned PostgresSchema recreated according to [schema.md](schema.md). The live Zettas must include
Zetta PR #1143 for the new CRUD names, but Lake still builds against the authorized transport
checkout.
