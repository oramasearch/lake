# AGENTS.md

Read `CLAUDE.md` first. It is the instruction file for this repository and applies in full to
every agent. Then `README.md` and `docs/README.md`.

Non-negotiables, restated from `CLAUDE.md`:

- Every change documents itself in `docs/<topic>/*.md` **in the same commit**, and updates
  `docs/design/status.md` when its status lines stop being true.
- **No AI attribution anywhere.** No `Co-Authored-By`, no "Generated with", no session or
  model trailers, in commits or pull requests.
- No time estimates. The word "seam" is banned; name the mechanism.
- Work on a feature branch named for the issue, from `dev`; open a pull request against `dev` that
  references the issue with `Refs #N`, and never push to `main` directly.
- Zetta is read-only reference material, read from the checkout `LAKE_ZETTA_SOURCE_DIR` names.
  Never copy Zetta source in.
- Build and test: `cmake -S . -B build -G Ninja && cmake --build build -j 6 && ctest --test-dir build --output-on-failure`.
