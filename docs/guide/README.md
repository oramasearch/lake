# The Docs section

These files are the Docs customers read at `/docs` (lake#252), linked as **Docs**, the last item under **Developer** in the navigation (for everyone, signed in or not). They are embedded into the binary at
build time by `cmake/embed_guide.cmake`, in the order its `GUIDE_FILES` lists (`index` is Getting
started, then `concepts`, `load-data`, `train`, `ship`, `agents`, `developer`), and rendered by
`lake::api::markdown_to_html` (`include/lake/api/markdown.hpp`: headings, lists, notes, code, tables,
inline code, bold, emphasis and safe links; everything else is escaped). A list item's continuation
lines join it before inline markup, so an inline code span may wrap across source lines.

Rules for writing them:

- Write for a customer who has never seen lake. Every step names where to click, then the REST call
  and the MCP tool for the same step.
- Describe what runs today. A capability that is planned is not in the Docs. `concepts.md` says
  when it was last checked against the running service; change that date when you check it again.
  `Workspaces.StartWorkspaceAnswersItsIdBesideTheSpacesOtherWorkspaces` keeps it saying a space has
  many workspaces.
- Keep Ship's ApplicationAgent steps explicit about reviewing the exact proposal before any write.
- Keep Train's ExperimentsAgent steps explicit about editing and confirming the exact study before
  it starts.
- A new page is a new `.md` here **and** its slug in `GUIDE_FILES`; the first line is `# <Title>`.
- Every REST/MCP example is complete and runs as pasted: it carries every required field, including
  `customer_id` and `workspace_id` (as `<customer id>` and `<workspace id>`). Write it as JSON next
  to its capability (`POST /v1/<tool>` or the tool's name in a code span);
  `Examples.EveryDocsExampleRunsAgainstTheRegistrySchema` (tests/test_examples.cpp, lake#439)
  validates each one against the registry's input schema.
- `Docs.EveryGuideRendersWithContentsAndTheNextGuide` (tests/test_markdown.cpp) renders every page.
