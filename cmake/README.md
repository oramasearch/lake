# CMake helpers

`embed_experiments.cmake` embeds the vendored uPlot distribution and Lake chart controller/styles
as a generated C++ translation unit. Asset content determines immutable URLs. Provenance and
update checks are in [assets/experiments](../assets/experiments/README.md) and the
[UI contract](../docs/ui/experiments.md).

`embed_fonts.cmake` converts the five checked-in IBM Plex WOFF2 files into a generated C++
translation unit. `lake-core` compiles that output, allowing the single Lake binary to serve its
own fonts without a runtime asset directory. The source files, licensing, update procedure, and
HTTP contract are documented in [`docs/ui/design-system.md`](../docs/ui/design-system.md).
