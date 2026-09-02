# CLAUDE.md — lake

The lake binary. **Org-wide conventions** live in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`
and apply in full. Project conventions live in
[`oramasearch/liquid`](https://github.com/oramasearch/liquid) `CLAUDE.md` — vocabulary,
document rules, confidentiality, and the decisions not to drift past. Read both.

## Status

**Bootstrap.** The specification is in `oramasearch/liquid/docs/features/`. The Phase 0
gate is lifted; hello world comes first (bridge `docs/action-plan.md`, Phase 0.5).

## Where to read Zetta

lake builds on Zetta, so you will read Zetta constantly. **Read it only from the umbrella
bridge:**

```
/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
```

Never from a standalone clone — one under `~/dev/oramasearch/zetta` was 1,053 commits
behind and lacked the entire HTTP layer lake borrows. The web-serving stack to copy is
`include/zetta/api/server.hpp` + `src/core/api/http_server.cpp` (Beast2 on capy/corosio,
one adapter TU) and the pure renderers in `include/zetta/api/web_ui.hpp`.

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
- Deployed as a `Lake` CR on `oramasearch/orama-operator` (the Zetta operator, extended); runs on `oramasearch/k0s`, and on our AKS cluster for now.

## Never

- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
