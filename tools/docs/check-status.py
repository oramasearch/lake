#!/usr/bin/env python3
"""Check docs/design/status.md against an observed OpenAPI document (lake#448).

Every row of the page's *Area status* table names the capabilities it speaks for in its
*Capabilities* column. The check fails when:

- a row says it is not deployed (its Deployed cell starts with "No") while the observed OpenAPI
  lists one of its capabilities, or says it is deployed ("Yes") while one is missing;
- a Deployed cell starts with neither "Yes" nor "No", or a Capabilities cell names none
  without saying "—" (a row that has no capability, such as the pages, is unchecked);
- a row names a capability the source tree's registry does not have (with --registry);
- the page does not link the observation it is checked against (with --require-link).

--observed takes a file or an http(s) URL. Either a full OpenAPI document (as served at
GET /v1/openapi.json) or a snapshot written by --snapshot is accepted, so the same check runs
offline in ctest against the committed snapshot and against the live service:

    tools/docs/check-status.py --observed https://lake.oramadev.net/v1/openapi.json

--snapshot PATH writes the observed capability names as a snapshot file and exits.
"""

import argparse
import datetime
import json
from pathlib import Path
import re
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def load_json(source):
    if re.match(r"^https?://", source):
        # An explicit agent: the edge in front of lake refuses Python's default one.
        request = urllib.request.Request(source, headers={"User-Agent": "lake-check-status/1"})
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    return json.loads(Path(source).read_text())


def capability_names(document):
    """The capability names a document lists: a snapshot's list or an OpenAPI's /v1/<name> paths."""
    if "capabilities" in document:
        return set(document["capabilities"])
    return {
        path[len("/v1/"):]
        for path in document.get("paths", {})
        if path.startswith("/v1/") and path.count("/") == 2
    }


def split_row(line):
    cells = line.strip().strip("|").split("|")
    return [cell.strip() for cell in cells]


def area_rows(text):
    """The Area status table as dictionaries keyed by header, with their line numbers."""
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == "## Area status")
    except StopIteration:
        raise SystemExit("status.md has no '## Area status' section")
    header = None
    rows = []
    for number, line in enumerate(lines[start + 1:], start + 2):
        if line.startswith("## "):
            break
        if not line.startswith("|"):
            continue
        cells = split_row(line)
        if header is None:
            header = cells
            continue
        if all(set(cell) <= set("-: ") for cell in cells):
            continue
        if len(cells) != len(header):
            raise SystemExit(f"status.md:{number}: {len(cells)} cells, header has {len(header)}")
        rows.append((number, dict(zip(header, cells))))
    if header is None or "Capabilities" not in header or "Deployed" not in header:
        raise SystemExit("status.md's Area status table needs Deployed and Capabilities columns")
    return rows


def check(status_path, observed, registry=None, require_link=None):
    text = Path(status_path).read_text()
    errors = []
    if require_link and Path(require_link).name not in text:
        errors.append(
            f"status.md does not link {Path(require_link).name}, the newest observation; "
            "re-check the page against it and link it"
        )
    for number, row in area_rows(text):
        area = row.get("Area", "?")
        deployed = row["Deployed"]
        names = re.findall(r"`([a-z0-9_]+)`", row["Capabilities"])
        if not names and not row["Capabilities"].startswith("—"):
            errors.append(f"status.md:{number} {area}: Capabilities names none and is not '—'")
        if registry is not None:
            for name in names:
                if name not in registry:
                    errors.append(f"status.md:{number} {area}: `{name}` is not in the registry")
        if deployed.startswith("No"):
            for name in names:
                if name in observed:
                    errors.append(
                        f"status.md:{number} {area}: says not deployed, "
                        f"but the observed OpenAPI lists `{name}`"
                    )
        elif deployed.startswith("Yes"):
            for name in names:
                if name not in observed:
                    errors.append(
                        f"status.md:{number} {area}: says deployed, "
                        f"but the observed OpenAPI does not list `{name}`"
                    )
        else:
            errors.append(f"status.md:{number} {area}: Deployed must start with Yes or No")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--status", default=str(ROOT / "docs/design/status.md"))
    parser.add_argument("--observed", required=True, help="OpenAPI or snapshot: a path or URL")
    parser.add_argument("--registry", help="the source tree's OpenAPI document (a path)")
    parser.add_argument("--require-link", help="a snapshot file the page must link")
    parser.add_argument("--snapshot", help="write the observed names to this path and exit")
    parser.add_argument("--revision", default="", help="the observed revision, for --snapshot")
    args = parser.parse_args()

    document = load_json(args.observed)
    observed = capability_names(document)
    if args.snapshot:
        snapshot = {
            "source": args.observed,
            "observed_on": datetime.date.today().isoformat(),
            "revision": args.revision,
            "capabilities": sorted(observed),
        }
        Path(args.snapshot).write_text(json.dumps(snapshot, indent=1) + "\n")
        print(f"wrote {len(observed)} capabilities to {args.snapshot}")
        return 0
    registry = capability_names(load_json(args.registry)) if args.registry else None
    errors = check(args.status, observed, registry, args.require_link)
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print(f"status.md agrees with {len(observed)} observed capabilities")
    return 0


if __name__ == "__main__":
    sys.exit(main())
