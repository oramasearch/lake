# tools/api

- `vendor-engine-openapi.py` (lake#650) writes `src/api/engine_openapi.json`, the workspace
  engine's input schemas and descriptions for the capabilities Lake allows (`workspace_reads` and
  `workspace_actions`), from Zetta's generated `schemas/openapi.json` (`--zetta-source`) or a
  document a workspace engine serves (`--openapi`). `--check` exits 1 when the snapshot differs.
  The build embeds the snapshot; each named engine capability publishes its schema
  ([docs/api/engine-capabilities.md](../../docs/api/engine-capabilities.md)).
