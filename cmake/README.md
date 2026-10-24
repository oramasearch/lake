# CMake helpers

`embed_fonts.cmake` converts the five checked-in IBM Plex WOFF2 files into a generated C++
translation unit. `lake-core` compiles that output, allowing the single Lake binary to serve its
own fonts without a runtime asset directory; the hosted application sign-in pages link them through
the stylesheet. The source files, licensing, update procedure, and HTTP contract are documented in
[`docs/ui/design-system.md`](../docs/ui/design-system.md).

`embed_schema.cmake` embeds `db/schema.sql` as one raw string (`lake::control::embedded_schema_sql`),
which `lake migrate` applies and `lake manifest` describes (lake#1119,
[`docs/deployment/operator-interface.md`](../docs/deployment/operator-interface.md)).

`embed_guide.cmake` embeds the Docs guides (`docs/guide/*.md`) in reading order; the `list_docs`
and `get_doc` capabilities read them, and the web app renders them.

The web app itself is embedded by `tools/ui/embed-frontend.mjs` (see
[`docs/ui/web-app.md`](../docs/ui/web-app.md)). The Experiments charts' uPlot is the app's npm
dependency; lake#561 retired `embed_experiments.cmake` and `assets/experiments/` with the
server-rendered pages.
