#!/usr/bin/env python3
"""The agents' live scenario suite: every library agent, over the agent bus, as a person (lake#269).

Each scenario is a test with a pass condition and a GitHub issue (label `test`) that records its
definition and results. It runs against a deployed lake through the public API, as the person whose
key it holds, and never waits inside a request: every turn is followed with receive_agent_events.

    tools/e2e/agents.py <base-url> <token-file> <customer-id> [scenario ...]

<token-file> holds a lake API key with every tool (Developer → MCP). The token is never printed. The
workspace needs the UX-review datasets (datasets.ux_review_churn and churn_results.at_risk_*).
Scenarios that only propose change nothing; nothing here confirms a proposal. Prints one line per
scenario and exits 1 when any failed. docs/api/agent-bus.md describes the bus.
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

base, token_file, customer, *only = sys.argv[1:]
token = open(token_file).read().strip()
if not token.lower().startswith("bearer "):
    token = "Bearer " + token


def api(method, capability, body=None):
    url = f"{base.rstrip('/')}/v1/{capability}"
    headers = {"Authorization": token, "User-Agent": "lake-e2e-agents", "Accept": "application/json"}
    data = None
    if method == "GET" and body:
        url += "?" + urllib.parse.urlencode(
            {k: v if isinstance(v, str) else json.dumps(v) for k, v in body.items()})
    elif body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode()
    request = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        return json.loads(error.read() or b"{}")


def turn(agent, message, kind="api", context=None, budget=330):
    """One agent turn, followed on the bus. Returns (task, seconds, request seconds, event types)."""
    started = time.time()
    asked = api("POST", "agent_turn", {"customer_id": customer, "agent": agent, "message": message,
                                       "kind": kind, "context": context or {}})
    request_seconds = time.time() - started
    if not asked.get("ok"):
        return {"status": "refused", "message": json.dumps(asked.get("error"))}, 0, request_seconds, []
    task_id, sequence, events, task = asked["value"]["task_id"], 1, [], {}
    while time.time() - started < budget:
        value = api("GET", "receive_agent_events", {"customer_id": customer, "task_id": task_id,
                                                    "from_sequence": sequence, "wait_ms": 10000}
                    ).get("value") or {}
        events += [event["type"] for event in value.get("events", [])]
        sequence = value.get("next_sequence", sequence)
        task = value.get("task") or task
        if task.get("status") in ("succeeded", "failed"):
            break
    return task, time.time() - started, request_seconds, events


def reads(task):
    answer = task.get("answer") or {}
    return [(read.get("capability"), bool(read.get("ok"))) for read in answer.get("reads", [])]


def base_checks(task, request_seconds, limit):
    """What every turn must do: answer the request at once, and finish over the bus in time."""
    problems = []
    if request_seconds > 2:
        problems.append(f"the request took {request_seconds:.1f}s (a turn never waits inside one)")
    if task.get("status") != "succeeded":
        problems.append(f"status {task.get('status')}: {task.get('message')}")
    return problems


def failed_reads(task, most):
    failed = [name for name, ok in reads(task) if not ok]
    return [f"{len(failed)} failed reads ({', '.join(failed)}), at most {most}"] if len(failed) > most else []


def t1_datasets():
    task, seconds, request, _ = turn("agent-ask-orama", "Which datasets do I have?")
    reply = (task.get("answer") or {}).get("reply") or ""
    problems = base_checks(task, request, 60) + failed_reads(task, 1)
    if "ux_review_churn" not in reply:
        problems.append("the reply does not name ux_review_churn")
    return problems, seconds


def t2_columns():
    task, seconds, request, _ = turn(
        "agent-ask-orama", "What columns does the ux_review_churn dataset have, and how many rows?")
    reply = (task.get("answer") or {}).get("reply") or ""
    problems = base_checks(task, request, 60) + failed_reads(task, 1)
    for word in ("churned", "tenure_months", "200"):
        if word not in reply:
            problems.append(f"the reply does not say {word}")
    return problems, seconds


def t3_ranked():
    top = api("GET", "workspace_sql", {"customer_id": customer, "statement":
              "SELECT account, risk FROM churn_results.at_risk_f3cced7b ORDER BY risk DESC LIMIT 3"})
    if not top.get("ok"):
        return [f"the workspace cannot rank: {top.get('error')}"], 0
    expected = str(top["value"]["rows"][0]["account"])
    task, seconds, request, _ = turn("agent-ask-orama",
                                     "Which of my accounts are most at risk of churning?")
    reply = (task.get("answer") or {}).get("reply") or ""
    problems = base_checks(task, request, 90) + failed_reads(task, 1)
    if "limit of tool rounds" in reply:
        problems.append("it ran out of tool rounds")
    if expected not in reply:
        problems.append(f"the reply does not name the riskiest account ({expected})")
    return problems, seconds


def t4_propose():
    task, seconds, request, _ = turn("agent-ask-orama", "Create a repository called e2e-agents-notes")
    answer = task.get("answer") or {}
    problems = base_checks(task, request, 30)
    if [p.get("capability") for p in answer.get("proposals", [])] != ["create_repository"]:
        problems.append(f"proposals {answer.get('proposals')}, want one create_repository")
    if "propos" not in (answer.get("reply") or "").lower():
        problems.append("the reply does not say it is proposed")
    return problems, seconds


def t5_delegate():
    task, seconds, request, _ = turn(
        "agent-ask-orama", "Ask the Project Agent to design a churn pipeline on my ux_review_churn "
        "dataset, then tell me what it said.")
    answer = task.get("answer") or {}
    problems = base_checks(task, request, 150)
    delegated = [d for d in answer.get("delegations", []) if d.get("agent") == "agent-project"]
    if not delegated:
        problems.append("it did not delegate to agent-project")
    elif (delegated[0].get("answer") or {}).get("status") != "succeeded":
        problems.append(f"the subagent's task: {delegated[0].get('answer')}")
    return problems, seconds


def t6_scan():
    task, seconds, request, events = turn("agent-autosuggest", "What could I build from my data?",
                                          "discover")
    problems = base_checks(task, request, 180)
    profile = ((task.get("answer") or {}).get("result") or {}).get("project_profile") or {}
    if not profile.get("suggested"):
        problems.append("nothing suggested")
    listed = api("GET", "list_opportunities", {"customer_id": customer, "task_id": task.get("task_id", "")})
    rows = [row for row in (listed.get("value") or {}).get("opportunities", [])
            if row.get("task_id") == task.get("task_id")]
    if not rows:
        problems.append("the scan's Opportunities were not kept")
    if "progress" not in events:
        problems.append("no progress event")
    return problems, seconds


def t7_build():
    task, seconds, request, _ = turn("agent-project",
                                     "Build a pipeline that predicts churn from my churn dataset")
    result = (task.get("answer") or {}).get("result") or {}
    problems = base_checks(task, request, 150)
    if result.get("applied") is not False or not result.get("pipeline"):
        problems.append("no pipeline design, or it claims to have applied it")
    return problems, seconds


def t8_factory():
    task, seconds, request, _ = turn(
        "agent-creator", "Create an agent that each Monday lists my accounts whose churn risk rose",
        "factory")
    answer = task.get("answer") or {}
    problems = base_checks(task, request, 90)
    reply = answer.get("reply") or ""
    try:
        kind = json.loads(reply).get("kind") if reply.startswith("{") else None
    except ValueError:
        kind = None
    if kind not in ("question", "proposal") and (answer.get("result") or {}).get("kind") not in (
            "question", "proposal"):
        problems.append("the answer is neither a question nor a proposal")
    return problems, seconds


def t9_bus_refuses_people():
    problems = []
    for capability, body in (("receive_agent_tasks", {"agent": "agent-project"}),
                             ("claim_agent_task", {"task_id": "00000000-0000-4000-8000-000000000000",
                                                   "agent": "agent-project"})):
        answer = api("GET" if capability.startswith("receive") else "POST", capability, body)
        if (answer.get("error") or {}).get("code") != "unauthorized":
            problems.append(f"{capability} as a person: {answer}")
    return problems, 0


def t10_workspace_pubsub():
    topic, problems = f"e2e-{int(time.time())}", []

    def act(capability, body, read=False):
        return api("POST", "workspace_query" if read else "workspace_act",
                   {"customer_id": customer, "capability": capability, "input": body})

    steps = [act("create_topic", {"topic": topic, "exposed": True}),
             act("publish_event", {"topic": topic, "payload": '{"hello":1}', "encoding": "json"})]
    received = act("receive_events", {"topic": topic, "from_sequence": 1, "wait_ms": 100}, True)
    steps += [received, act("drop_topic", {"topic": topic})]
    for step in steps:
        if not step.get("ok"):
            problems.append(json.dumps(step.get("error")))
    if not problems and len((received.get("value") or {}).get("events", [])) != 1:
        problems.append("the published event was not received")
    return problems, 0


SCENARIOS = {
    "T1": ("Ask Orama lists the datasets", t1_datasets),
    "T2": ("Ask Orama describes one dataset", t2_columns),
    "T3": ("Ask Orama answers a ranked data question", t3_ranked),
    "T4": ("Ask Orama proposes a change and never makes it", t4_propose),
    "T5": ("Ask Orama delegates to the Project Agent and follows its task", t5_delegate),
    "T6": ("Scan my workspace keeps its Opportunities", t6_scan),
    "T7": ("The Project Agent designs a pipeline without applying it", t7_build),
    "T8": ("The Agent Factory asks or proposes", t8_factory),
    "T9": ("A person cannot reach the agents' side of the bus", t9_bus_refuses_people),
    "T10": ("A workspace's pub/sub works through lake's API", t10_workspace_pubsub),
}

failed = 0
for key, (title, run) in SCENARIOS.items():
    if only and key not in only:
        continue
    problems, seconds = run()
    failed += bool(problems)
    print(f"{key} {'PASS' if not problems else 'FAIL'} {seconds:6.1f}s  {title}"
          + ("" if not problems else "\n      - " + "\n      - ".join(problems)), flush=True)
sys.exit(1 if failed else 0)
