# Design and status

[status.md](status.md) records disabled-account agent offboarding (lake#951), including immediate
credential refusal and durable workspace-stream cleanup; [Clerk](../api/clerk.md#webhooks) has the
behavior and verification contract.
[status.md](status.md) records the workspace dataset/model metadata authorization fix (lake#970) separately from deployment; [models.md](../api/models.md) specifies the access contract.
[PKI SAN validation](../api/pki.md#certificate-identities) is implemented in the source tree
(lake#946); [status.md](status.md) keeps that fix separate from deployed PKI behavior.
[status.md](status.md) records bounded pipeline processing (lake#967); the
[API contract](../api/workspaces.md#pipeline-work-budgets) defines limits and worker-pool refusal.
The [model gateway](../api/model-gateway.md#supported-requests) restricts provider requests to
text and caller-executed functions (lake#960); [status.md](status.md) records source support.

[status.md](status.md) tracks bounded JSON admission (lake#944) separately from deployment;
[HTTP limits](../serving/http.md#request-body-limits) defines its credential and parser budgets.

Clerk/OIDC bounded JWKS refresh and the Clerk exchange attempt limit (lake#956) are tracked in [status.md](status.md) and specified in [Clerk](../api/clerk.md#bounded-key-refresh-lake956).

Secure-networking store and capability support and its separate operator rollout are tracked in
[status.md](status.md) and specified in the [control-plane](../control-plane/network.md) and
[API](../api/network.md) contracts.
Recipient-only [tunnel credentials](../api/tunnel-grants.md), migration 0085 and the pending
zetta#1281 history provider have source-tree support; live relay termination remains coordinator-owned.

Agent-task lease recovery is tracked separately from deployment in [status.md](status.md).
The cascading workspace deletion (lake#514) has its own row in [status.md](status.md).
The [agent bus contract](../api/agent-bus.md) documents retries and shared-credential limits.
Dispatcher authority and customer agent registration (lake#957) have their own source-tree row
in [status.md](status.md); the [agent-session contract](../api/agents.md) describes the checks.

Train → Experiments implementation, local proof and deployment acceptance are tracked separately
in [status.md](status.md); see its [API](../api/experiments.md) and [UI](../ui/experiments.md) contracts.

[overview.md](overview.md) describes Lake’s place in the product; [status.md](status.md) uses the
implemented, exercised locally, deployed, and exercised-in-deployment vocabulary. The dated
2026-10-01 record of the dev deployment is the source for
current deployment facts, and `tools/docs/check-status.py` fails when the page calls a
implemented, exercised locally, deployed, and exercised-in-deployment vocabulary. The newest observed OpenAPI in
[`../deployment/observed/`](../deployment/observed/) is the source for deployment facts, and `tools/docs/check-status.py` fails when the page calls a
capability undeployed that the newest observed OpenAPI lists (lake#448). Token attenuation and ancestor revocation are part of the identity status and contract in
[../api/identity.md](../api/identity.md).

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

Personal customer ownership on sign-up is specified in [../api/provisioning.md](../api/provisioning.md)
and reflected in [status.md](status.md).

The customer landing journey and viewer navigation are recorded in
[../ui/front-door.md](../ui/front-door.md) and reflected in [status.md](status.md).

Train → Streams is tracked separately from deployment in [status.md](status.md), with its
[API](../api/streams.md), [UI](../ui/streams.md) and ownership contracts linked there.

Ship Apps direct-zone hosts and reserved-name validation are tracked separately from deployment in
[status.md](status.md), with the [API contract](../api/web-apps.md) linked there.

Indexed PostgreSQL session authentication and the legacy-session migration are specified in
[../api/auth.md](../api/auth.md) and recorded separately from deployment in [status.md](status.md).

The design for lake as Orama's cloud (navigation by Repositories, Design, Train and Ship, and the
new home page) is [cloud.md](cloud.md), with a static mock in [ui/mocks/cloud/](ui/mocks/cloud/README.md).
It is not implemented.

- [`zetta-parity.md`](zetta-parity.md) — every Zetta capability reachable from lake; the pipeline as the central object (plan, 2026-09-26)

[status.md](status.md) records repository workspaces (lake#315 part 1, migration `0051`) as
implemented and not yet deployed.

[status.md](status.md) records forge records in the repository's workspace Zetta (lake#341, migration
`0054`) as implemented and not yet deployed.

[status.md](status.md) records repository environments and the platform mirror in the repository's
workspace Zetta (lake#358, lake#359, migration `0060`) as implemented and not yet deployed.

[status.md](status.md) records the audit split by subject (lake#317, lake#318,
migration `0063`) as implemented and not yet deployed.

[status.md](status.md) records the remote MCP/OAuth JSON nesting budget (lake#958);
[the contract](../api/mcp-connections.md#the-client) names the readers and regression tests.
Folder-sharing role attenuation and non-admin recipient restrictions (lake#955) are recorded in
[status.md](status.md) and [the sharing contract](../api/sharing.md).
`status.md` records organization owner-only membership changes (lake#965); the
[API contract](../api/organizations.md#delegated-administration) states the rule and checks.


Repository mutation destination and descendant authorization (lake#962) is recorded in
[status.md](status.md) and [the Drive contract](../api/drive.md).
`status.md` records the source-only dynamic-secret transparency privacy fix (lake#953): digest
commitments for successful calls and private audit for denials and failures.

`status.md` records checkpoint-authenticated log replication and refusal of legacy full-event
identity scans (lake#974); [replication](../api/replication.md) describes the compatibility
change and verification.
[status.md](status.md) records workspace audit queue isolation and its pending durable retry work.
Release source binding and its producer rollout requirement are recorded in
[status.md](status.md); the [forge contract](../api/forge.md#source-binding) specifies
which signed evidence can satisfy a protected push.
Storage quota changes through either setter require the operator or instance administrator
(lake#976); the [status table](status.md) records implementation and deployment separately.
