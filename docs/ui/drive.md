# Drive pages

The drive pages are server-rendered, self-contained HTML. A repository page links to its drive.
Folder rows link to another folder; supported files link to preview; every path links to history.
Folder sharing displays direct and inherited effective access, explains live/link/copy delivery,
creates expiring links, and revokes links with POST forms. No sharing state is inferred in the
renderer: the page receives the capability result.

Linux desktop presentation remains a liquid client concern. Lake documents the shared states:

| State | Hydration and offline behavior | Badge meaning |
|---|---|---|
| Online-only | Metadata is local; file bytes hydrate on open and require connectivity | Cloud outline |
| Pinned | The full selected subtree is hydrated and retained for offline reads | Filled pin/check |
| Locally modified | Local bytes remain available and await reconciliation; conflicts retain both values | Sync or conflict badge |

Pin changes control hydration policy, never repository retention. Offline writes are local changes
and publish through the same three-tree reconciler when connectivity returns. GNOME and KDE badge
and pin behavior is tracked by [liquid #196](https://github.com/oramasearch/liquid/issues/196).
