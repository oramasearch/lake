# The Docs section

These files are the Docs customers read at `/docs` (lake#252). They are embedded into the binary at
build time by `cmake/embed_guide.cmake`, in the order its `GUIDE_FILES` lists (`index` is Getting
started, then `concepts`, `load-data`, `train`, `ship`, `agents`, `developer`), and rendered by
`lake::api::markdown_to_html` (`include/lake/api/markdown.hpp`: headings, lists, notes, code, tables,
inline code, bold, emphasis and safe links; everything else is escaped).

Rules for writing them:

- Write for a customer who has never seen lake. Every step names where to click, then the REST call
  and the MCP tool for the same step.
- Describe what runs today. A capability that is planned is not in the Docs.
- A new page is a new `.md` here **and** its slug in `GUIDE_FILES`; the first line is `# <Title>`.
- `Docs.EveryGuideRendersWithContentsAndTheNextGuide` (tests/test_markdown.cpp) renders every page.
