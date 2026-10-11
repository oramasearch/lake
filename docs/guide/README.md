# The Docs section

These files are the Docs customers read at `/docs` (lake#252), linked as **Docs**, the last item
under **Developer** in the navigation (for everyone, signed in or not). They are embedded into the
binary at build time by `cmake/embed_guide.cmake`, in the order its `GUIDE_SECTIONS` lists, each
under the section the Docs navigation shows it in (lake#561). The `list_docs` and `get_doc`
capabilities serve them ([../api/web-app.md](../api/web-app.md#list_docs-and-get_doc)); the web
app renders them (`frontend/src/pages/developer/DocsPage.tsx`, [../ui/README.md](../ui/README.md)),
and the tests render them with `lake::api::markdown_to_html` (`include/lake/api/markdown.hpp`:
headings, lists, notes, code, tables, inline code, bold, emphasis and safe links; everything else
is escaped). A list item's continuation lines join it before inline markup, so an inline code span
may wrap across source lines.

## The pages

| Section | Slug | Title |
|---|---|---|
| Get Started | `index` | Getting Started (the first page, `/docs`) |
| | `concepts` | Concepts and Glossary |
| | `workspaces` | Workspaces |
| | `account` | Your Account and Sign-In |
| | `ask-orama` | Ask Orama |
| | `phone` | Using Lake on a Phone |
| Data | `repositories` | Repositories |
| | `repository-collaboration` | Reviews, Issues and Releases |
| | `repository-settings` | Branch Protection, Webhooks, Import and Export |
| | `drives` | Drives |
| | `sharing` | Sharing and Shared with Me |
| | `search` | Search |
| | `load-data` | Datasets and Loading Data |
| | `transcripts` | Transcripts and Speech-to-Text |
| Design | `design` | AI Projects and Pipelines |
| | `solutions` | AI Solutions, Methodologies and Opportunities |
| Train | `train` | Train a Model |
| | `agents` | Agents |
| | `experiments` | Experiments |
| | `streams` | Streams |
| Ship | `ship` | Models and Search Indexes |
| | `apps` | Apps |
| | `registry` | Packages, Builds and the Registry |
| | `deployments` | Deployments and Environments |
| | `services` | Services and Storage |
| Organization | `organizations` | Organizations, Members and Teams |
| | `access` | Grants and Effective Access |
| | `sso` | Single Sign-On and SCIM |
| | `tokens` | Tokens and Keys |
| | `secrets` | Secrets and Encryption Keys |
| | `activity` | Activity and Audit |
| | `usage` | Usage and Metering |
| | `billing` | Billing and Invoices (only where the lake bills; `list_docs` and `get_doc` leave it out where billing is off) |
| Developer | `developer` | REST API and OpenAPI |
| | `mcp` | MCP Setup |
| | `cli` | The liquid CLI |
| Reference | `security` | Security Model |
| | `faq` | FAQ and Troubleshooting |

## Rules for writing them

- Write for a customer who has never seen lake. Each area page says what it is for, its concepts,
  then step-by-step tasks, then limits, permissions, troubleshooting and **Related Pages**.
- Every step names where to click (the rail group and item, then the exact button label from the
  page's component in `frontend/src/pages/`), then the REST call and the MCP tool for the same step.
- Describe what runs today. A capability that is planned is not in the Docs; a limit may be stated
  plainly as a limit. `concepts.md` says when it was last checked against lake's source; change
  that date when you check it again. `Workspaces.StartWorkspaceAnswersItsIdBesideTheSpacesOtherWorkspaces`
  keeps it saying an account "can have many workspaces" and naming `workspace_id`.
- Speak of **workspaces**. The guides never say "space" or "personal space" to a reader; `customer_id`
  is the id of the account that owns the workspaces (theirs, or their organization's). Code spans
  and blocks may name fields and quote server messages. `Docs.GuidesSpeakOfWorkspaces` checks the
  prose, and the banned word "seam".
- Speak of **experiments**, as an AI experiment tracker does. The guides never say "study" to a
  reader; it is the engine's name, which only code spans may quote (`study-events.*`,
  ExperimentsAgent's `study` field). An experiment's attempts are **trials**, each a training
  **run**. `Docs.GuidesSpeakOfExperiments` checks the prose (lake#561).
- Headings are Title Case (lake's rule, `docs/ui/design-system.md` → *Capitalization*).
- Keep Ship's ApplicationAgent steps explicit about reviewing the exact proposal before any write.
- Keep Train's ExperimentsAgent steps explicit about editing and confirming the exact experiment
  before it starts.
- Name held-out metrics as the run report does (`held_out_auc`, …), and lead a run's report with
  its held-out score.
- A new page is a new `.md` here **and** its slug in a section of `GUIDE_SECTIONS`; the first line
  is `# <Title>`. A slug ending in `?` is optional (embedded only when its file exists).
  `Docs.EveryGuideFileIsEmbeddedOnceUnderASection` fails for a file left out, a slug listed twice
  or a section split in two.
- Link other guides as `/docs/<slug>` or `/docs/<slug>#<anchor>`, where the anchor is the heading's
  words in lower case joined by `-` (`markdown_anchor`). `Docs.LinksBetweenGuidesResolve` checks
  that every linked page and heading exists.
- Every REST/MCP example is complete and runs as pasted: it carries every required field, including
  `customer_id` and `workspace_id` (as `<customer id>` and `<workspace id>`). Write it as JSON next
  to its capability (`POST /v1/<tool>` or the tool's name in a code span), or as
  `GET /v1/<tool>?<inputs>`; `Examples.EveryDocsExampleRunsAgainstTheRegistrySchema`
  (tests/test_examples.cpp, lake#439) validates each one against the registry's input schema.
  Every JSON object in code after a capability is named counts as that capability's input, so show
  no answers as JSON there; put a client configuration or an error envelope before the first
  capability on the page, or describe it in prose.
- `Docs.EveryGuideHasItsTitleAndRenders` (tests/test_markdown.cpp) checks every page's title and
  renders it; `get_doc` and `list_docs` (sections, headings, `q`) are tested in tests/test_api.cpp.
