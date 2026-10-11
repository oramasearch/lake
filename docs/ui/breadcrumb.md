# The top-bar breadcrumb

**What.** The top bar of every page in the shell shows where the page sits, as a clickable trail
that mirrors the URL (lake#205): *Design › Projects › retention-review › churn*. Every level but
the last is a link; the last is the current page (`aria-current="page"`). The last level also
gives the document title, `lake — <title>`, unless the page names its own.

| Page | Breadcrumb |
|---|---|
| `/design/projects` | Design › **Projects** |
| `/design/project?name=retention-review` | Design › Projects › **retention-review** |
| `/design/pipeline?project=retention-review&graph=churn` | Design › Projects › retention-review › **churn** |
| `/design/opportunities`, `/design/discover` | Design › **Opportunities** |
| `/train/agents`, `/train/agent?id=…` | Train › **Agents**; Train › Agents › **the abbreviated agent id** |
| `/train/agents/new` | Train › Agents › **Agent Factory** |
| `/ask` | **Ask Orama** |
| `/train/data`, `/train/query` | Train › **Data**; Train › Data › **Query** |
| `/train/dataset?name=accounts` | Train › Data › **accounts** |
| `/connections`, `/connections/connection?name=warehouse` | Data › **Connections**; Data › Connections › **warehouse** (lake#651) |
| `/train/run?id=3c9e1a2b-…` | Train › Runs › **3c9e1a2b…** (full id on hover) |
| `/ship/indexes`, `/ship/index?id=…` | Ship › **Search Indexes**; Ship › Search Indexes › **5b42a297…** |
| `/ship/app?app_id=…` | Ship › Apps › **the app's name** |
| `/services` | Ship › **Services** |
| `/workspaces` | Ship › **Workspaces** |
| `/changes?repository_id=…` | Repositories › owner/name › **Changes** |
| `/organization?id=…` | Organization › Organizations › **the organization's display name** (Administration for the operator) |
| `/tokens` | Account › **Tokens** |

**How it is built.**

- **The default trail** is built by the shell (`components/Shell.tsx`) from the route registry:
  the rail group (linked to its first item), the rail item that owns the path (`currentNav` in
  `routes.tsx`, using each screen's `owns`), then the screen's title when it adds a level. Account
  pages for a signed-in person start with Account (linked to `/settings`). A page with no
  entities in its URL needs nothing more.
- **A page that knows its entities** replaces the default with `useTrail([...], title)` from
  `lib/trail.tsx`, and the shell shows it while the page is mounted. The shared helpers build the
  usual shapes: `useDesignTrail` (`pages/design/parts.tsx`: Design › the list page › the
  entities), `useRepositoryTrail` (`pages/repositories/forge.tsx`: Repositories › the repository,
  linking to its home › what the page is) and `useMetaTrail` (`pages/organizations/shared.tsx`:
  group › item › the page's subject, for the organization and administration pages).
- **Entities by name, else by abbreviated id.** `entityCrumb` shows a name as typed. A UUID, or 16
  or more hex characters, shows its first eight characters and an ellipsis, with the whole value
  in `title` for hover.
- **Links keep the workspace.** Design, Train and Ship levels link under the current `/w/<id>/`.

**Rules.**

- **Title Case** for fixed labels ([design-system.md](design-system.md#capitalization)); names are
  never re-cased.
- **Phones** show the last two levels (`lake.css`, `max-width: 40rem`).

**Where it differs from the rail.** The Train group's link is not the same on every page:
Agents, Data, Dataset, Query, Runs and Run link *Train* to Data; Streams and Experiments link it to
Runs; the default trail follows the rail and links it to Agents; on Agent Factory *Train* is not a
link. Ship links to Models everywhere.

**How to verify.** Open each row of the table above and hover an abbreviated level for its full
value. `tools/e2e/walkthrough.mjs` screenshots show the trail on every page it walks
([../deployment/browser-testing.md](../deployment/browser-testing.md)).

**How to change.** A new rail item gets its breadcrumb for free, and so does a screen that its
list page `owns`. A new page with an entity in its URL calls `useTrail` (or one of the helpers
above) with `entityCrumb` for that entity's level.
