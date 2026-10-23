# Safe Mode

**Status: implemented 2026-10-23 in the source tree (lake#906); not deployed. Needs the workspace
Zetta's safe mode (zetta#1373) in the workspace image; until then the two capabilities answer
`not_supported` and no call is warned.**

Every database of a workspace, and every saved connection, has a **safe mode**, modelled on the one
in Bobby Tables (Steve Sperandeo, 2026-10-23, on lake#906): *"you can have safe mode off, warn
writes, warn all ... let's do the same so we can warn with the queries that we're going to run, and
can control it / change it per database."*

| Level | `mode` | What it does |
|---|---|---|
| Off | `off` | Every statement runs without asking. |
| Warn Writes | `warn_writes` | **The default.** A statement that changes data is refused with `safe_mode_warning` until confirmed. Remote sources are read-only, so their reads run without a prompt. |
| Warn All | `warn_all` | Every statement is refused with `safe_mode_warning` until confirmed. |

A warning is a confirmation by the person running the call, not an approval by someone else.

## Where the level lives

In the workspace Zetta, never in Lake. Lake passes `get_safe_mode` and `set_safe_mode` through to
the workspace like its other engine capabilities ([engine-capabilities.md](engine-capabilities.md))
and stores no copy. The workspace audits each change and each confirmed warning; Lake's own
workspace audit event for a call repeated with a confirmation carries `safe_mode_confirmed: true`.

## Read and set the level

| Capability | Kind | Permission | Input | Answer |
|---|---|---|---|---|
| `get_safe_mode` | read | `read` on the account | `database` or `connection` | `{mode}` |
| `set_safe_mode` | action | `write` on the account | `database` or `connection`, and `mode` (`off`, `warn_writes`, `warn_all`) | the workspace's answer |

Both also take `customer_id` and `workspace_id`, and both are reachable through `workspace_query`
and `workspace_act` with `capability: "get_safe_mode"` or `"set_safe_mode"`. A `mode` outside the
three is refused by Lake as `invalid_input` at `/mode` before the workspace is called.

```sh
curl -s "https://lake.example/w/$WS/v1/get_safe_mode?customer_id=$C&workspace_id=$WS&connection=warehouse" \
  -H "Authorization: Bearer $TOKEN"
# {"ok":true,"value":{"mode":"warn_writes"},"duration_ms":4.1}

curl -s https://lake.example/w/$WS/v1/set_safe_mode -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"customer_id":"'$C'","workspace_id":"'$WS'","database":"datasets","mode":"warn_all"}'
```

## The warning

When a call would run a statement its database's or connection's level warns on, the workspace
refuses it and Lake answers the refusal unchanged, on REST (`409`) and MCP (`isError: true`, the
same envelope in `structuredContent`):

```json
{ "ok": false,
  "error": { "code": "safe_mode_warning",
             "message": "…",
             "path": "",
             "details": { "connection": "warehouse",
                          "mode": "warn_all",
                          "statements": ["SELECT * FROM accounts"],
                          "confirmation": "9f3a1c…" } },
  "duration_ms": 6.2 }
```

`details` names the `database` or the `connection`, the `mode`, the exact `statements` that would
run and a `confirmation`. Show the statements to the person; to run them, send **the same call
again** with `safe_mode_confirmation` set to `confirmation`. A confirmation answers one warning: if
the workspace warns again (the statements changed), show the new ones.

## Calls that take `safe_mode_confirmation`

| Call | How it is passed |
|---|---|
| `workspace_sql` | a top-level field; Lake forwards it to the workspace's `sql` |
| `list_source_tables`, `create_dataset_from_query` | a top-level field of the named capability |
| every other named engine capability whose input takes `database` or `connection` (`analyze_dataset`, `create_index`, `decide`, `get_dataset`, `plan_features`, `promote_dataset`, `upload_dataset`) | a top-level field; sent only after the workspace warned |
| `workspace_query`, `workspace_act` | inside `input`, as the engine takes it |

## Verify

- `SafeMode.TheLevelIsReadAndSetInTheWorkspace` (`tests/test_workspaces.cpp`): both capabilities
  reach the workspace by name and through `workspace_query`/`workspace_act`, with the input as sent;
  a made-up level never reaches it.
- `SafeMode.AWarningCarriesItsStatementsAndTheConfirmationRunsTheCall`: the warning keeps its
  details on REST and MCP for `list_source_tables`, `workspace_act`'s `create_dataset_from_query` and
  `workspace_sql`, the repeated call carries the confirmation, and other refusals carry no details.

## Change it

- The level names and the selector are the workspace's; Lake describes `get_safe_mode` and
  `set_safe_mode` itself (`lake_described` in `src/api/engine_capabilities.cpp`) until the vendored
  engine schema (`src/api/engine_openapi.json`) carries them, then the engine's schema wins.
- The confirmation field's schema is `safe_mode_confirmation_schema()`
  (`src/api/workspace_capabilities.cpp`); the error is `safe_mode_warning` in
  `documented_error_codes` (`src/api/registry.cpp`), its details kept by `workspace_failure`.
- The web app: [ui/safe-mode.md](../ui/safe-mode.md).
