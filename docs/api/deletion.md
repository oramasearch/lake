# Deleting and revoking (lake#645)

Before lake#645 a person could create organizations, secret mounts, datasets, models, runs, search
indexes, invitations, grants and Ask Orama conversations, but could not remove them through the app,
REST or MCP; the only way out was raw SQL. Each now has a capability in the one registry (REST,
OpenAPI, MCP), and each:

- is policy-checked by `Registry::call` like every capability, and audited: control-plane objects
  in the central log, a workspace's data in that workspace's own audit chain (decision 52,
  [audit.md](audit.md#which-log));
- takes `dry_run: true`, which changes nothing and answers what would go, as `delete_workspace`
  does ([workspaces.md](workspaces.md#deletion));
- is protected in Lake Console by a typed confirmation where the item lives, the dialog listing
  what goes from the dry run first ([../ui/deletion.md](../ui/deletion.md));
- removes a workspace's data from its workspace Zetta; nothing of it is left behind.

| Capability | Input | What goes | Log | Contract |
|---|---|---|---|---|
| `delete_organization` | `organization_id`, `confirm` (its handle), `dry_run?` | its teams, members, invitations, SSO providers, federation issuers, secret mount routes, and every grant on it and its space; refused while its space has workspaces, which the refusal names | central | [organizations.md](organizations.md#deleting-an-organization) |
| `revoke_invitation` | `invitation_id`, `dry_run?` | a pending invitation, which becomes `revoked` | central | [organizations.md](organizations.md#revoking-an-invitation) |
| `revoke_grant` | `grant_id`, `dry_run?` | one grant; not one a membership carries, nor a space's last admin grant | central | [policy.md](policy.md#revoking-a-grant) |
| `delete_secret_mount` | `mount_id`, `confirm` (its name), `dry_run?` | every secret and version, named key, data key and lease of the mount, its name claim and its record, then its route and policy object | workspace | [secrets.md](secrets.md#deleting-a-mount) |
| `delete_dataset` | `customer_id`, `workspace_id`, `id` (`<database>/<dataset>`), `dry_run?` | the dataset's table, rows and every search index on it | workspace | [models.md](models.md#deleting-from-a-workspace) |
| `delete_model` | … `id` (or `model_id`) | a trained model, no longer served; its run is kept | workspace | [models.md](models.md#deleting-from-a-workspace) |
| `delete_run` | … `id` (or `run_id`) | a finished run and its report; the models it trained are kept | workspace | [models.md](models.md#deleting-from-a-workspace) |
| `delete_index` | … `id` (or `index_id`) | a search index, no longer served; its dataset is kept | workspace | [models.md](models.md#deleting-from-a-workspace) |
| `delete_ask_conversation` | `customer_id`, `workspace_id`, `conversation_id`, `dry_run?` | every message of one of the caller's conversations | workspace | [ask-orama.md](ask-orama.md#deleting-a-conversation) |

## Why an organization is marked, not erased

The central audit chain names the organization's space (`audit_events.customer_id`, `ON DELETE
RESTRICT`), and an audit event is never rewritten. So `delete_organization` removes everything the
organization holds and every way to reach it, and marks the `organizations` row and its space's
`customers` row deleted (migration `0076_organization_deletion`, `deleted_at`). Lake never lists or
reads a deleted one again: `get_organization`, `organization_customer`, `get_customer` answer
`not_found`, and the lists leave them out. The handle stays retired. Its workspaces were deleted
first (each with `delete_workspace`, which removes their Zettas), so no workspace data remains.

## Datasets, models, runs and indexes need the engine's delete

The four engine deletions are lake capabilities that call the workspace engine's own
`delete_dataset`, `delete_model`, `delete_run` and `delete_index` (zetta#1257). A workspace whose
engine is older answers `501 not_supported` with what it needs, and nothing is deleted. They are
named capabilities, not only reachable through `workspace_act`, so the person's permissions and the
workspace audit name exactly what was deleted.

## Verify

`tests/test_deletions.cpp` (`Stores/Deletions.*`, over the in-memory store and PostgreSQL): an
organization is refused while it owns a workspace and the plan names it, then deleted with its
members, invitations and grants and gone from every read; a revoked invitation cannot be accepted; a
grant is revoked unless a membership or the last admin needs it; a secret mount leaves none of its
records in its workspace and the mount beside it still opens; a conversation leaves only other
conversations' messages; the engine deletions send the engine's fields, answer `not_found` at `/id`,
and `not_supported` from an older engine; and each capability is in the log its subject belongs to.
