# CLAUDE.md — lake

The lake binary. **Org-wide conventions** live in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`
and apply in full. Project conventions live in
[`oramasearch/liquid`](https://github.com/oramasearch/liquid) `CLAUDE.md` — vocabulary,
document rules, confidentiality, and the decisions not to drift past. Read both.

## Status

**Design. No code.** The specification is in `oramasearch/liquid/docs/features/`. This
repository holds the implementation once the Zetta/liquid storage decision is made
(liquid `CLAUDE.md`, *Decisions not to drift past*). Until then, do not add source,
build files or CI here.

## When code arrives

- **C++26**, CMake ≥ 3.28 + Ninja, Catch2 — matching `oramacore-cpp` and `zetta`.
- `std::expected` at every public boundary; no exceptions across public APIs.
- Shares the chunk store and CRDT datastore with liquid. There is one store, not two.
- Deployed by `oramasearch/lake-operator`; runs on `oramasearch/k0s`.

## Never

- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
