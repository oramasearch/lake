# Deleting and Revoking in Lake Console

lake#645. Everything a person can create in Lake Console can also be removed there: an
organization, an invitation, a grant, a secret mount, a dataset, a trained model, a run, a search
index and an Ask Orama conversation. Each one is removed where it lives, through one confirmation
dialog, and through the capability REST and MCP callers use ([../api/deletion.md](../api/deletion.md)).

## The confirmation

`frontend/src/lib/deletionDialog.tsx` is the one dialog (`DeletionDialog`, and `DeletionButton`, its
trigger). It works the way the workspace deletion on Workspaces does
([workspace-selection.md](workspace-selection.md#the-workspaces-page)):

1. On opening, it calls the capability with `dry_run: true`. That call deletes nothing and refreshes
   nothing on screen (`postRead`), and it is not asked again while the deletion runs.
2. It lists what goes from that answer, one line per kind with its count, and what is kept.
3. The danger button stays disabled until the person types the confirmation text exactly.
4. Only then does it call the capability for real. `act` refreshes every read on screen, a toast
   says it is done, and the page goes back to its list where the item had its own page.

A refusal (a membership's grant, a run still running, an engine that cannot delete yet) stays in
the dialog as Lake said it. `not_supported` from an older workspace engine is shown, never retried.
`frontend/src/lib/deletion.ts` turns each capability's dry-run answer into the dialog's lines and
names the confirmation text; it is pure and tested by `frontend/tests/deletion.test.ts`.

## Where each one is

| What | Where | Typed to confirm | What the dialog lists |
|---|---|---|---|
| Organization | Organization → header, **Delete organization**, for an owner (or the Lake operator) | its handle | members, teams, invitations, access grants, SSO providers and federation issuers that go; while it owns a workspace, the action is disabled and each workspace is linked to its own deletion on Workspaces |
| Invitation | Organization → Invitations tab and `/invitations`, **Revoke** on a pending invitation that has not expired | `revoke` | who can no longer accept it; it stays listed as revoked |
| Grant | `/grants?object_id=` (Direct Grants), **Revoke** per grant; Effective Access links each source to its direct grants | `revoke` | the role removed; Lake refuses a grant that comes with a membership and an account's last admin grant |
| Secret mount | Secret Mounts, **Delete** per mount | the mount's name | secrets with their versions, encryption keys, data keys, leases, and every secret by name |
| Dataset | Data → Dataset, Actions card, **Delete dataset** | the dataset's name | rows and size, the search indexes built on it |
| Model | Ship → Model, header, **Delete** (one version) | the model's name | the trained model, whether it was published, the run it keeps |
| Run | Train → Run, header, **Delete**, enabled once the run has finished | the first 8 characters of its id | the run and its report, the models it keeps |
| Search index | Ship → Search Index, header, **Delete** | the indexed table (its id's first 8 characters without one) | the index, whether it was published, the dataset it keeps |
| Ask Orama conversation | Ask Orama home, beside New conversation, **Delete** | `delete` | how many messages, how it starts, how many agent tasks it started go with it; a new conversation starts after it |
| Your account (lake#810) | Settings → Profile, **Delete account** | your handle | your workspaces, the organizations you alone own and those you leave, sessions and tokens, credentials and devices, access grants, the billing contact, and that invoices and the audit log are kept; while you alone own an organization other people belong to, the action is disabled and the organization is linked. After it, you are signed out ([settings.md](settings.md#deleting-your-account)) |

## How to check it

`frontend/checks/deletion.mjs` drives the built pages with Playwright against `npx vite` and mocked
answers, at 1440 and 390 px wide: an organization that still owns a workspace (blocked, the
workspace linked), one that owns none (the typed handle enables the button), a secret mount, a
dataset, an Ask Orama conversation with the two agent tasks it started (lake#673), and your own
account, blocked by an organization you alone own with other members and then free to go (lake#810). It reports whether a page scrolls sideways, the dialog's lines and the button's state
before and after typing, and writes screenshots to `OUT`.
