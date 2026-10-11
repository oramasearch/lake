#!/usr/bin/env python3
"""Vendor the workspace engine's input schemas into lake (lake#650).

Every capability a lake caller may reach in a workspace engine (the `workspace_reads` and
`workspace_actions` allow-lists in src/api/workspace_capabilities.cpp) is a named capability in
lake's registry, and its input schema and description come from the engine's own OpenAPI document,
never from a guess. This tool reads that document and writes the allow-listed operations to
src/api/engine_openapi.json, which the build embeds (cmake/embed_engine_openapi.cmake).

The document is Zetta's generated, drift-checked `schemas/openapi.json` from the Zetta source tree
lake builds against (LAKE_ZETTA_SOURCE_DIR), or any copy a workspace engine serves
(`GET /workspaces/<customer_id>/zetta/v1/openapi.json` through lake), passed with --openapi.

    tools/api/vendor-engine-openapi.py --zetta-source ../zetta          # write the snapshot
    tools/api/vendor-engine-openapi.py --zetta-source ../zetta --check  # exit 1 when it differs

The engine's descriptions are copied with Lake's wording rules applied (WORDING below): the same
input always gives the same snapshot, and --check compares after them.
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ALLOW_LISTS = ROOT / "src/api/workspace_capabilities.cpp"
SNAPSHOT = ROOT / "src/api/engine_openapi.json"


def allow_list(source: str, name: str) -> list[str]:
    match = re.search(name + r" = \{(.*?)\};", source, re.S)
    if not match:
        sys.exit(f"no {name} in {ALLOW_LISTS}")
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return re.findall(r'"([a-z_0-9]+)"', body)


# Lake's wording rules (lake#647: never "space", never a lowercase product "lake" in prose) applied
# to the engine's text, exactly and in order, so the vendored copy reads as Lake's own text does.
# Each rewrites a string Zetta itself now words this way (oramasearch/zetta, the agent definition
# and study preflight descriptions); once the Zetta source carries it, the rewrite finds nothing.
WORDING = [
    ('runtime: python. {source: "zetta", instructions, entrypoint, files: [{path, contents}]} '
     '(inline, at most 1 MiB) or {source: "lake", repository_id, revision, entrypoint}; '
     'entrypoint is module:function',
     'runtime: python. `{source: "zetta", instructions, entrypoint, files: [{path, contents}]}` '
     '(inline, at most 1 MiB) or `{source: "lake", repository_id, revision, entrypoint}`; '
     'entrypoint is `module:function`'),
    ("zetta (inline) or lake", "`zetta` (inline) or `lake`"),
    ("source lake: ", 'with `source: "lake"`: '),
    ("source zetta: ", 'with `source: "zetta"`: '),
    ("the methodology's default search space", "the methodology's default parameter ranges"),
]


def lake_wording(value):
    if isinstance(value, dict):
        return {key: lake_wording(child) for key, child in value.items()}
    if isinstance(value, list):
        return [lake_wording(child) for child in value]
    if isinstance(value, str):
        for engine, lake in WORDING:
            value = value.replace(engine, lake)
    return value


def without_draft_marker(schema):
    if isinstance(schema, dict):
        return {key: without_draft_marker(value) for key, value in schema.items() if key != "$schema"}
    if isinstance(schema, list):
        return [without_draft_marker(value) for value in schema]
    return schema


def input_schema(method: str, operation: dict) -> dict:
    """The operation's input as one object schema, as the engine validates it."""
    if method == "post":
        schema = operation.get("requestBody", {}).get("content", {}).get("application/json", {})
        return without_draft_marker(schema.get("schema", {"type": "object"}))
    properties, required = {}, []
    for parameter in operation.get("parameters", []):
        schema = dict(parameter.get("schema", {}))
        if "description" in parameter and "description" not in schema:
            schema["description"] = parameter["description"]
        properties[parameter["name"]] = without_draft_marker(schema)
        if parameter.get("required"):
            required.append(parameter["name"])
    schema = {"type": "object", "properties": properties, "additionalProperties": False}
    if required:
        schema["required"] = required
    return schema


def zetta_revision(source: pathlib.Path | None) -> str:
    if source is None:
        return "unknown"
    try:
        return subprocess.run(
            ["git", "-C", str(source), "log", "-1", "--format=%h", "--", "schemas/openapi.json"],
            capture_output=True, text=True, check=True).stdout.strip() or "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--zetta-source", type=pathlib.Path,
                        help="the Zetta source tree (reads schemas/openapi.json)")
    parser.add_argument("--openapi", type=pathlib.Path,
                        help="an engine OpenAPI document, instead of the Zetta source tree's")
    parser.add_argument("--check", action="store_true",
                        help="compare with the vendored snapshot instead of writing it")
    arguments = parser.parse_args()
    if arguments.openapi is None and arguments.zetta_source is None:
        parser.error("pass --zetta-source or --openapi")
    path = arguments.openapi or arguments.zetta_source / "schemas/openapi.json"
    document = json.loads(path.read_text())

    source = ALLOW_LISTS.read_text()
    reads = allow_list(source, "workspace_reads")
    actions = allow_list(source, "workspace_actions")
    capabilities = {}
    missing = []
    for name in dict.fromkeys(reads + actions):
        operations = document.get("paths", {}).get("/v1/" + name, {})
        method = "get" if "get" in operations else "post" if "post" in operations else None
        if method is None:
            missing.append(name)
            continue
        operation = operations[method]
        capabilities[name] = {
            "method": method,
            "summary": operation.get("summary", ""),
            "description": operation.get("description", ""),
            "input_schema": input_schema(method, operation),
        }
    snapshot = {
        "about": "The workspace engine's input schemas for the capabilities lake allows "
                 "(lake#650). Generated by tools/api/vendor-engine-openapi.py; do not edit.",
        "engine_version": document.get("info", {}).get("version", ""),
        "zetta_revision": "unknown" if arguments.openapi else zetta_revision(arguments.zetta_source),
        "capabilities": lake_wording(capabilities),
    }
    text = json.dumps(snapshot, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    if arguments.check:
        current = json.loads(SNAPSHOT.read_text()) if SNAPSHOT.exists() else {}
        current.pop("zetta_revision", None)
        snapshot.pop("zetta_revision", None)
        if current != snapshot:
            print(f"{SNAPSHOT.relative_to(ROOT)} differs from {path}; run this tool without --check")
            return 1
        print(f"{SNAPSHOT.relative_to(ROOT)} matches {path}")
        return 0
    SNAPSHOT.write_text(text)
    print(f"wrote {len(capabilities)} capabilities to {SNAPSHOT.relative_to(ROOT)}")
    if missing:
        print("the engine publishes no schema for: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
