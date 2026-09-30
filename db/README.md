# Database schema

`schema.sql` is the idempotent PostgreSQL source of truth. Migration `0022_session_lookup`
adds a unique public session selector and revokes legacy sessions without a selector once.
New session rows store SHA-256 of a random bearer. Apply it before the new binary. The operator
must vendor the same bytes and use `session_lookup_guard`; that handoff is a separate change.
See `docs/control-plane/schema.md` and `docs/api/auth.md` for rollout and replacement credentials.

Migrations `0034_web_apps` and `0035_web_apps_operator_contract` add the App desired/observed row,
runtime mode, optional source entrypoint and operator-written token hashes. Their rollout guard is
`web_apps_operator_contract_guard`. Apply before exposing Apps; the upgrade removes the obsolete
credential table. Lake never stores app tokens. See `docs/control-plane/web-apps.md`.

Migration `0036_builds` adds the queued/cancelled image-build row and operator observation fields.
Its guard is `builds_guard`; desired writes notify `lake_builds`, and only SHA-256 hashes of
the operator's source, push and event credentials are stored. See `docs/control-plane/builds.md`.

Migration `0037_streams` adds durable topic creator ownership keyed by customer, workspace
service ID and topic. Its guard is `streams_guard`. Vendor/apply the schema in the
operator rollout before exposing Streams; see `docs/control-plane/streams.md`.

Migration `0040_active_repository_names` replaces the repository table constraint with the
partial unique index `repository_name_unique`, which applies only while `deleted_at IS NULL`.
Deleting a repository therefore releases its customer-scoped name while preserving the tombstone.
Apps, services, stores and workspaces already use equivalent live-row predicates for their name
indexes.

Migration `0041_app_forward_auth` adds Lake-owned, short-lived authorization-code state for the
protected-app browser handoff. Only SHA-256 code hashes are stored; exchange is single-use and
expired or spent rows are swept. Its guard is `app_forward_auth_guard`. See
`docs/api/web-apps.md` and `docs/control-plane/web-apps.md`.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Migration `0042_agent_task_leases` adds renewable leases, bounded attempts and pending retry
notices to `agent_task_status`, with partial indexes for expiry and notice delivery. Legacy
Running rows receive a lease once. The latest guard is `agent_task_leases_guard`; the operator
must vendor this schema and update its guard before rollout. The contract, idempotency checks
and deferred operator configuration are in [agent-bus.md](../docs/api/agent-bus.md).
