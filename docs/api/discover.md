# Design → Opportunities

**Updated 2026-09-27 (lake#269):** Discover became **Design → Opportunities** (`/design/opportunities`;
`/design/discover` still answers). *Scan my workspace* is a background Auto-Suggest turn
(`agent_turn`, kind `discover`); the page follows it and the Opportunities it recommends (and the
ones it declines, *Not yet*) are **kept** as rows of `opportunities`, across scans. Each can become a
project draft (`create_projects_from_opportunities {opportunity_ids}`, on confirmation) or be dismissed
(`dismiss_opportunity`). `list_opportunities {task_id?}` reads them over REST and MCP. Each names the
catalogue solution it maps to; the catalogue itself in lake is lake#270.

# Discover Opportunities

**Status: implemented 2026-09-27 (lake#237, epic lake#228).** Design's *Discover Opportunities*
(`/design/discover`, linked beside *New Project*), backed by the Agent Library's Auto-Suggest
([`oramasearch/agent-autosuggest`](https://github.com/oramasearch/agent-autosuggest)). It follows
Studio's discovery journey (orama-design `src/studio/discovery/`).

1. **Scan my workspace** (`POST /design/discover/scan`) sends one `agent_turn` to
   `agent-autosuggest`. lake builds the **Workspace Profile** from the approved data the workspace
   holds (`list_datasets`, then `get_dataset` for up to twelve: names and columns), so Auto-Suggest
   reads nothing itself and never changes anything.
2. **Recommended Opportunities**: Auto-Suggest's `suggested` entries, each with its intent, solution
   id, methodology, why this data supports it, what must come first, and what it changes; **Not Yet**:
   its `declined` entries with the reason and *capture it first* or *cannot be done*.
3. **Create project drafts** (`POST /design/discover/create`) needs the person's explicit
   confirmation. Each selected Opportunity becomes a project whose intent is the Opportunity's and
   whose description carries its **Source Opportunity** (solution, methodology, evidence). Nothing
   runs until a plan is approved.

When Auto-Suggest is not running on the lake (`LAKE_AGENT_ENDPOINTS`), the page says so and offers
nothing behind a mock. A scan that fails says why.

Verify: `HttpServer/HttpResponseIsolation.DiscoverOpportunitiesScansAndCreatesDraftsOnlyOnConfirmation`.
