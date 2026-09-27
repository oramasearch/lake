# The top-bar breadcrumb

**What.** The top bar of every page shows where the page sits, as a clickable, multi-level trail
that mirrors the URL (lake#205): *Design › Projects › retention-review › churn*. Every level but
the last is a link; the last is the current page (`aria-current="page"`). It replaces the single
plain word (`meta`) the top bar showed before.

| Page | Breadcrumb |
|---|---|
| `/design/projects` | Design › **Projects** |
| `/design/project?name=retention-review` | Design › Projects › **retention-review** |
| `/design/pipeline?project=retention-review&graph=churn` | Design › Projects › retention-review › **churn** |
| `/train/agents`, `/train/agent?id=…` | Train › **Agents**; Train › Agents › **the abbreviated agent id** |
| `/train/agents/new` | Train › Agents › **Agent Factory** |
| `/design/discover` | Design › Projects › **Discover Opportunities** |
| `/ask` | **Ask Orama** |
| `/train/data`, `/train/query` | Train › **Data**; Train › Data › **Query** |
| `/train/dataset?name=accounts` | Train › Data › **accounts** |
| `/train/run?id=3c9e1a2b-…` | Train › Runs › **3c9e1a2b…** (full id on hover) |
| `/ship/indexes`, `/ship/index?id=…` | Ship › **Search Indexes**; Ship › Search Indexes › **5b42a297…** |
| `/services` | Ship › **Services** |
| `/changes?repository_id=…` | Repositories › owner/name › **Changes** |
| `/organization?id=…` | Organization › Organizations › **Acme** |
| `/tokens` | Account › **Tokens** |

**Rules.**

- **Levels come from the sidebar.** A page's first levels are its sidebar group and item, read from
  the same `nav_items` list the sidebar renders, so the two never disagree. A group links to its
  first item (Design → Projects, Train → Data, Ship → Models).
- **Entities by name, else by abbreviated id.** An entity's level shows its name as typed. An
  id-like value (a UUID, or 16 or more hex characters) shows its first eight characters and an
  ellipsis, with the whole id in `title` for hover (`web_ui_entity_crumb`).
- **Links keep the space.** Design, Train and Ship levels carry the page's `customer_id`.
- **Title Case** for fixed labels (docs/ui/design-system.md, *Capitalization*); names are never
  re-cased.
- **Phones** show the last two levels.
- No client JavaScript: it is part of the server-rendered shell.

**How it is built.** `WebUiCrumb {label, href, full}` and `web_ui_breadcrumb(trail)` in
`include/lake/api/web_ui.hpp`. `web_ui_shell(title, meta, …)` builds the default trail
(`default_trail`: sidebar group and item, then `meta` when it adds a level; account pages under
Account). Pages that know their entities pass a trail to the `web_ui_shell(title, trail, …)`
overload: the Design, Train and Ship pages (`workspace_trail`) and repository pages
(`repository_shell`: Repositories › the repository, linking to its home › what the page is).

**How to verify.** `WebUi.BreadcrumbLinksEveryLevelButTheCurrentOne` and
`Workspaces.BreadcrumbMirrorsTheUrlWithNamesAndAbbreviatedIds`; in a browser,
`tools/e2e/walkthrough.mjs` screenshots show the trail on every page.

**How to change.** A new sidebar item gets its breadcrumb for free. A new page with an entity in
its URL passes a trail built with `web_ui_entity_crumb` for that entity's level.
