# CLAUDE.md — lake

The lake binary. **Org-wide conventions** live in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`
and apply in full. Project conventions live in
[`oramasearch/liquid`](https://github.com/oramasearch/liquid) `CLAUDE.md` — vocabulary,
document rules, confidentiality, and the decisions not to drift past. Read both.

## Status

**Bootstrap.** The specification is in `oramasearch/liquid/docs/features/`. The Phase 0
gate is lifted; hello world comes first (bridge `docs/action-plan.md`, Phase 0.5).

## What lake is built on

**lake is the enterprise product and builds on Zetta as much as possible** — control
plane, operational state, clustering, and **its web-serving approach**. lake must also
host liquid repositories, so it consumes **liquid's store as a library** for repository
data. liquid is open source and never depends on Zetta; lake sits on the other side of
that licence boundary.

- **C++26**, CMake ≥ 3.28 + Ninja, Clang 17+ auto-selected as Zetta's `CMakeLists.txt`
  does, Catch2.
- **Boost.Capy** coroutines, **Corosio** networking — via `oramasearch/corosio`, never
  `cppalliance` directly.
- `std::expected` at every public boundary; no exceptions across public APIs.
- Deployed by `oramasearch/lake-operator`; runs on `oramasearch/k0s`.

## Never

- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
