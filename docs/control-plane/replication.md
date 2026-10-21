# Multi-region replication operator handoff

Lake owns the repository and transparency-log replication protocol. The Orama operator owns the
topology, credentials, scheduling, fencing, and repeated reconciliation. This page is the contract
for the operator change; the operator source is outside this repository.

## Desired state

A two-region `Lake` resource identifies exactly one active region and one or more standbys:

```yaml
apiVersion: orama.cloud/v1alpha1
kind: Lake
metadata:
  name: production
spec:
  regions:
    - name: region-a
      role: active
      endpoint: https://region-a.lake.example
    - name: region-b
      role: standby
      endpoint: https://region-b.lake.example
  replication:
    credentialSecretName: lake-peer-replication
```

The operator creates one Lake deployment and persistent data root in each region. It maps the
Secret key `token` to `LAKE_REPLICATION_TOKEN` in every deployment. That credential must be an
operator credential accepted by every peer and must not appear in the custom resource status. Lake
never falls back to the admin token when it is missing (lake#915).

It also sets `LAKE_REPLICATION_PEERS` in each deployment to the other regions' endpoints
(origins, comma separated). A standby refuses to replicate from any source not in that list, so
the replication credential is only sent to those endpoints
([`../api/replication.md`](../api/replication.md#peers-and-the-replication-credential)). Peer
endpoints are HTTPS on public addresses.
The instance `--node` value is the stable region name.

The operator also provisions a control-plane topology in which customer, repository, policy, Lake
identity, and audit rows are available in each region. Lake's data-plane protocol does not
replicate Postgres.

## Reconciliation

For every live repository, the reconciler calls `replicate_repository` on each standby with the
active endpoint and instance name. It then calls `replicate_transparency_log` until `caught_up` is
true. Reconciliation is level-triggered: failed calls remain pending, and repeated successful
calls are idempotent. The operator records per-standby conditions containing the latest repository
cursor, log tree size and root, checkpoint sequence, last success timestamp, and sanitized error.

The operator does not make a standby writable merely because replication is current. Ingress and
write-capable credentials target only the active deployment. Promotion requires:

1. fencing the former active from writes and checkpoint publication;
2. reconciling the candidate until all required repositories and the log report current;
3. checking that its adopted checkpoint matches a witnessed checkpoint retained outside either
   region; and
4. changing the candidate role and traffic routing as one controlled operation.

A log `unrecoverable` response blocks promotion. The operator preserves both persistent volumes
and surfaces the condition for investigation; it must not delete, overwrite, or truncate either
log.

## Network policy and credentials

Standbys need outbound HTTPS to the active Lake endpoint. The active must accept the protected
replication export and repository sync routes from standby identities. Witness and anchor egress
remain governed by the transparency configuration in [`log.md`](log.md).

Rotate the replication Secret by first making the new credential valid on all sources, then
rolling standbys, then removing the old credential. Lake logs only whether a replication token is
configured.

## Verification

Create a repository and append a transparency entry through the active. Reconcile the standby,
then verify:

- `replicate_repository` reports refs and a durable cursor;
- a pull from the standby has the same liquid root as a pull from the active;
- `replicate_transparency_log` returns `caught_up: true` and the same tree size and root;
- `transparency_checkpoint` on the standby returns the source checkpoint after it becomes local;
  and
- the audit stream contains successful `replication` events for repository and log actions.

The repository integration test starts two Lake servers with distinct store roots and checks
convergence and a zero-byte retry. The transparency test uses distinct instance directories,
checks incremental convergence, and proves that a modified declared root is refused before state
publication.
