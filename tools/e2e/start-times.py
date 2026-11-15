#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Measure how long each service kind takes to start, phase by phase (lake#197).

Creates services through lake's API, follows them every second through list_services, reads the
pods' own Ready times from Kubernetes, deletes them, and prints each phase and the total, then the
median and p90 per kind. No browser.

    tools/e2e/start-times.py <base-url> <token-file> <customer-id> <runs> <kind> [kind ...]

<token-file> holds a lake API token able to call create_service, list_services and delete_service
for <customer-id> (Developer → MCP, "Every tool"). kubectl must reach the cluster; the context is
LAKE_KUBE_CONTEXT (required) and the Lake's namespace LAKE_NAMESPACE (default lake), the Lake
LAKE_NAME (default prod). The token is never printed.

Phases, each measured from Create:
  seen        the operator wrote the first status (it noticed the new row)
  pods        the last of the service's pods reported Ready (Kubernetes' own transition time)
  ready       lake shows the service Ready (the operator found it ready and delivered the credential)
  connected   lake can read the credential (the Services page shows Ready, not Connecting)
"""
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime

base, token_file, customer, runs, *kinds = sys.argv[1:]
token = open(token_file).read().strip()
context = os.environ.get("LAKE_KUBE_CONTEXT") or sys.exit("start-times: set LAKE_KUBE_CONTEXT to the cluster's kubectl context")
namespace = os.environ.get("LAKE_NAMESPACE", "lake")
lake = os.environ.get("LAKE_NAME", "prod")


def api(method, capability, body=None, query=None):
    url = f"{base}/v1/{capability}"
    if query:
        url += "?" + urllib.parse.urlencode(query)
    data = json.dumps(body).encode() if body is not None else None
    # An explicit User-Agent: the edge in front of lake refuses Python's default one.
    request = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {token}", "Content-Type": "application/json",
        "User-Agent": "lake-start-times/1"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body, status = response.read(), response.status
    except urllib.error.HTTPError as error:
        body, status = error.read(), error.code
    try:
        return json.loads(body)
    except ValueError:
        return {"ok": False, "error": {"status": status, "body": body[:200].decode(errors="replace")}}


def pods_ready_at(prefix):
    """The latest Ready transition among the pods whose name starts with prefix, as epoch seconds."""
    out = subprocess.run(["kubectl", f"--context={context}", "-n", namespace, "get", "pods", "-o", "json"],
                         capture_output=True, text=True, check=True).stdout
    latest = None
    for pod in json.loads(out)["items"]:
        if not pod["metadata"]["name"].startswith(prefix + "-"):
            continue
        for condition in pod.get("status", {}).get("conditions", []):
            if condition["type"] == "Ready" and condition["status"] == "True":
                at = datetime.fromisoformat(condition["lastTransitionTime"].replace("Z", "+00:00")).timestamp()
                latest = at if latest is None else max(latest, at)
    return latest


def measure(kind, run):
    name = f"st-{kind}-{int(time.time()) % 100000}"
    created = time.time()
    answer = api("POST", "create_service", {"customer_id": customer, "kind": kind, "name": name,
                                             "instances": 1, "storage_gi": 1})
    if not answer.get("ok"):
        return {"kind": kind, "run": run, "error": answer.get("error")}
    service = answer["value"]
    sid, slug = service["id"], service["slug"]
    seen = ready = connected = None
    messages = []
    deadline = created + 20 * 60
    while time.time() < deadline:
        listed = api("GET", "list_services", query={"customer_id": customer})
        row = next((s for s in listed.get("value", {}).get("services", []) if s.get("id") == sid), None)
        now = time.time() - created
        if row:
            phase, message = row.get("phase") or "", row.get("message") or ""
            if phase and seen is None:
                seen = now
            if message and (not messages or messages[-1][1] != message):
                messages.append((round(now, 1), message))
            if phase == "Ready" and ready is None:
                ready = now
            if phase == "Ready" and row.get("connected", True):
                connected = now
                break
            if phase == "Failed":
                break
        time.sleep(1)
    pods_at = pods_ready_at(f"{lake}-{slug}")
    api("POST", "delete_service", {"customer_id": customer, "service_id": sid, "confirm": name})
    return {"kind": kind, "run": run, "seen": seen, "pods": pods_at - created if pods_at else None,
            "ready": ready, "connected": connected, "messages": messages}


def fmt(value):
    return "—" if value is None else f"{value:6.1f}s"


results = []
for run in range(1, int(runs) + 1):
    for kind in kinds:
        result = measure(kind, run)
        results.append(result)
        if "error" in result:
            print(f"{kind:10} run {run}: create failed: {result['error']}", flush=True)
            continue
        print(f"{kind:10} run {run}: seen {fmt(result['seen'])}  pods {fmt(result['pods'])}  "
              f"ready {fmt(result['ready'])}  connected {fmt(result['connected'])}", flush=True)
        for at, message in result["messages"]:
            print(f"{'':10}   {at:6.1f}s  {message}", flush=True)
        # Let the deletion finish before the next service of the same kind claims its quota slot.
        time.sleep(20)

print("\nkind        runs  median  p90   (connected: Create → usable)")
for kind in kinds:
    totals = sorted(r["connected"] for r in results if r["kind"] == kind and r.get("connected") is not None)
    if not totals:
        print(f"{kind:10}  0")
        continue
    p90 = totals[min(len(totals) - 1, int(round(0.9 * (len(totals) - 1))))]
    print(f"{kind:10}  {len(totals):4}  {statistics.median(totals):5.0f}s {p90:5.0f}s")
json.dump(results, open(os.environ.get("START_TIMES_OUT", "/dev/null"), "w"), indent=2)
