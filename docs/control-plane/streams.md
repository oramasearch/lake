# Stream ownership and bus isolation

Migration `0037_streams` adds `stream_owners(customer_id,workspace_id,topic,created_by)` and
`streams_guard`. The composite primary key binds a concrete topic to the current workspace
service row, not merely a reusable space/name. The customer foreign key cascades deletion.
Only a successful Zetta `create_topic` establishes the recorded creator. A failed or ambiguous
creation never grants management of an existing topic. Successful topic drop removes ownership.
If Zetta succeeds but the following ownership store write fails, the operation reports failure;
the topic remains readable but cannot be reconfigured/dropped until ownership is reconciled.
Lake does not infer creator ownership from a service credential or a topic's name.
An existing ownership record refuses creation before contacting Zetta, including after an
out-of-band topic drop or failed ownership cleanup. A second person cannot recreate that name
under the first person's permissions; reconcile the leftover record before reusing the name.
The regression test grants the second principal space write access explicitly, then verifies
the ownership conflict is returned without an upstream call or recreated topic.

Memory and Postgres stores implement the same create/read/delete contract. The telemetry store
wrapper forwards it without changing semantics. `list_agent_task_stream_scopes(customer_id)`
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
must vendor `db/schema.sql` and advance its guard to `repository_names_guard` in its own rollout
change; this worktree does not modify the operator repository. An existing installation needs the
owned PostgresSchema recreated according to [schema.md](schema.md). The live Zettas must include
Zetta PR #1143 for the new CRUD names, but Lake still builds against the authorized transport
checkout.
