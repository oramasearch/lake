# Vendored dependencies

Lake records direct source dependencies as Git submodules so a checkout resolves the exact source
used by every build. Run `git submodule update --init --recursive` after cloning and after changing
branches. The recorded commits, rather than the configured branch names, determine a build.

| Path | Source | Update line |
|---|---|---|
| `modules/capy` | `cppalliance/capy` | upstream `develop` |
| `modules/corosio` | private `oramasearch/corosio` patch carrier | `orama-main` |
| `modules/http` | private `oramasearch/http` patch carrier | `zetta-patches` |
| `modules/beast2` | private `oramasearch/beast2` patch carrier | `orama-main` |
| `modules/libpqxx` | `jtv/libpqxx` | reviewed recorded commit |
| `modules/oramacore-cpp` | private `oramasearch/oramacore-cpp` | `main` |
| `modules/liquid` | private `oramasearch/liquid`, library-only | `main` |

`modules/liquid` owns its transitive submodules. Lake initializes them recursively but does not
select their revisions independently.

## Beast2 carrier

`oramasearch/beast2` is a private copy, not a GitHub fork. Its `main` branch mirrors public
upstream. Branch `orama-main` carries one reviewed commit on Lake's current Beast2 base
`fb0a9c8`: `fe1af5a`, which treats
`boost::http::error::end_of_stream` from an idle keep-alive close as a normal connection end and
does not print it to stderr. Lake records that commit in `modules/beast2` and declares
`branch = orama-main` in `.gitmodules` for update tooling.

Maintain the carrier in its own checkout. Give the public `upstream` remote a disabled push URL,
fast-forward carrier `main` to the reviewed public commit, then replay the carried patch:

```sh
git fetch upstream develop
git switch main
git merge --ff-only upstream/develop
git push origin main
git switch orama-main
git rebase main
git push --force-with-lease origin orama-main
```

If the patch no longer applies or upstream behavior has changed, stop and review the conflict
instead of dropping or rewriting the behavior silently. After the carrier branch is published,
advance Lake explicitly and verify the selected commit before recording the pointer:

```sh
git submodule sync -- modules/beast2
git -C modules/beast2 fetch origin orama-main
git -C modules/beast2 switch --detach origin/orama-main
git -C modules/beast2 rev-parse HEAD
git add modules/beast2
```

A fresh recursive clone is the consumer check; the native static and shared suites prove the
selected carrier against Lake.
