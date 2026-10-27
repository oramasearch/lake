#!/usr/bin/env python3
"""The agents' live scenario suite: every library agent, over the agent bus, as a person (lake#269).

Each scenario is a test with a pass condition and a GitHub issue (label `test`) that records its
definition and results. It runs against a deployed lake through the public API, as the person whose
key it holds, and never waits inside a request: every turn is followed with receive_agent_events.

    tools/e2e/agents.py <base-url> <token-file> <customer-id> <workspace-id> [scenario ...]

<token-file> holds a lake API key with every tool (Developer → MCP). The token is never printed. Every
workspace call names the workspace (`workspace_id`, required since lake v0.5). The scenarios read
`datasets.ux_review_churn` (200 accounts, with an `as_of` snapshot date, a 90-day churn window,
`churned` and `tenure_months`) and `datasets.ux_review_at_risk` (each account's risk); when the
workspace does not have them, the suite uploads them first (`upload_dataset`) and says so. T3 ranks
by a `churn_results.at_risk_*` table when a run wrote one, else by `ux_review_at_risk`.
Scenarios that only propose change nothing. T11 confirms its exact Application Agent proposal,
deploys that App, and requests its URL. T12 confirms an Experiments Agent experiment, observes its
trials, and asks for the explanation. Prints one line per scenario and exits 1 when any failed.
docs/api/agent-bus.md describes the bus.
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

if len(sys.argv) < 5 or sys.argv[4].upper().startswith("T") and sys.argv[4][1:].isdigit():
    sys.exit(__doc__)
base, token_file, customer, workspace, *only = sys.argv[1:]
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


def app_answers(url):
    request = urllib.request.Request(
        url, method="GET", headers={"Authorization": token, "User-Agent": "lake-e2e-agents"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return 200 <= response.status < 300 and bool(response.read())
    except urllib.error.HTTPError:
        return False


def turn(agent, message, kind="api", context=None, budget=330, extra=None):
    """One agent turn, followed on the bus. Returns (task, seconds, request seconds, event types)."""
    started = time.time()
    asked = api("POST", "agent_turn", {"customer_id": customer, "workspace_id": workspace, "agent": agent,
                                       "message": message, "kind": kind, "context": context or {},
                                       **(extra or {})})
    request_seconds = time.time() - started
    if not asked.get("ok"):
        return {"status": "refused", "message": json.dumps(asked.get("error"))}, 0, request_seconds, []
    task_id, sequence, events, task = asked["value"]["task_id"], 1, [], {}
    while time.time() - started < budget:
        value = api("GET", "receive_agent_events", {"customer_id": customer, "workspace_id": workspace,
                                                    "task_id": task_id, "from_sequence": sequence,
                                                    "wait_ms": 10000}
                    ).get("value") or {}
        events += [event["type"] for event in value.get("events", [])]
        sequence = value.get("next_sequence", sequence)
        task = value.get("task") or task
        if task.get("status") in ("succeeded", "failed"):
            break
    task.setdefault("task_id", task_id)
    return task, time.time() - started, request_seconds, events


def reads(task):
    answer = task.get("answer") or {}
    return [(read.get("capability"), bool(read.get("ok"))) for read in answer.get("reads", [])]


def confirming(task):
    """What a confirm turn names (lake#561): the turn that proposed, and its proposal's digest."""
    stored = api("GET", "get_agent_task", {"customer_id": customer, "workspace_id": workspace,
                                           "task_id": task["task_id"]}).get("value") or {}
    return {"proposal_task_id": task["task_id"], "proposal_sha256": stored.get("proposal_sha256", "")}


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
    top = api("GET", "workspace_sql", {"customer_id": customer, "workspace_id": workspace,
                                       "statement": RANKING})
    if not top.get("ok") or not (top.get("value") or {}).get("rows"):
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
    listed = api("GET", "list_opportunities", {"customer_id": customer, "workspace_id": workspace,
                                               "task_id": task.get("task_id", "")})
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
    """Train → Streams' capabilities (docs/api/streams.md): raw topic writes left workspace_act."""
    topic, problems = f"e2e-{int(time.time())}", []
    where = {"customer_id": customer, "workspace_id": workspace, "zetta": "workspace", "topic": topic}

    steps = [api("POST", "create_stream", where),
             api("POST", "publish_stream_message", {**where, "payload": '{"hello":1}'})]
    received = api("POST", "browse_stream", {**where, "from_sequence": 1, "direction": "forward",
                                             "limit": 10})
    steps += [received, api("POST", "delete_stream", {**where, "confirm_name": topic})]
    for step in steps:
        if not step.get("ok"):
            problems.append(json.dumps(step.get("error")))
    if not problems and len((received.get("value") or {}).get("events", [])) != 1:
        problems.append("the published event was not received")
    return problems, 0


def t11_application_agent():
    app_name = f"e2e-health-{int(time.time())}"
    task, seconds, request, _ = turn(
        "agent-application",
        f"Build a lake-authenticated FastAPI data app named {app_name} that reads "
        "datasets.ux_review_churn and answers with a churn summary at its root URL.",
        "app")
    answer = task.get("answer") or {}
    proposal = answer.get("result") or {}
    problems = base_checks(task, request, 150)
    if proposal.get("kind") != "proposal" or proposal.get("confirm") is not True:
        problems.append(f"the first turn did not return a confirmed proposal: {proposal}")
        return problems, seconds
    confirmed, confirm_seconds, confirm_request, _ = turn(
        "agent-application",
        "Confirm and deploy the reviewed proposal.",
        "app",
        {"confirmed_proposal": proposal},
        420, confirming(task))
    seconds += confirm_seconds
    deployed = (confirmed.get("answer") or {}).get("result") or {}
    problems += base_checks(confirmed, confirm_request, 420)
    if deployed.get("kind") != "deployed":
        problems.append(f"the confirmed turn did not deploy: {deployed}")
        return problems, seconds
    app = deployed.get("app") or {}
    url = app.get("url") or (app.get("rollout") or {}).get("url")
    if not url:
        problems.append("the deployed result has no URL")
    elif not app_answers(url):
        problems.append(f"the deployed URL did not answer: {url}")
    return problems, seconds


def t12_experiments_agent():
    task, seconds, request, _ = turn(
        "agent-experiments",
        "Tune a churn model on datasets.ux_review_churn for validation AUC with no more than "
        "four trials, one trial at a time, and a fixed seed.",
        "experiment")
    answer = task.get("answer") or {}
    proposal = answer.get("result") or {}
    problems = base_checks(task, request, 180)
    if proposal.get("kind") != "proposal" or proposal.get("confirm") is not True:
        problems.append(f"the goal did not produce a confirmable proposal: {proposal}")
        return problems, seconds
    if not isinstance(proposal.get("study"), dict):
        problems.append("the proposal has no editable experiment (the agent's study object)")
        return problems, seconds

    confirmed, confirm_seconds, confirm_request, _ = turn(
        "agent-experiments",
        "Confirm and start the reviewed experiment proposal.",
        "experiment",
        {"confirmed_proposal": proposal},
        420, confirming(task))
    seconds += confirm_seconds
    running = (confirmed.get("answer") or {}).get("result") or {}
    problems += base_checks(confirmed, confirm_request, 420)
    # Experiments Agent answers with the engine's word for the experiment, `study`.
    experiment = running.get("study") or {}
    experiment_id = experiment.get("id")
    if running.get("kind") != "running" or not experiment_id:
        problems.append(f"the confirmed proposal did not start an experiment: {running}")
        return problems, seconds

    trials = experiment.get("trials") or []
    status_result = {}
    for _ in range(5):
        if trials:
            break
        status, status_seconds, status_request, _ = turn(
            "agent-experiments",
            f"Report the current trials for experiment {experiment_id}.",
            "experiment_status",
            {"experiment_id": experiment_id},
            180)
        seconds += status_seconds
        problems += base_checks(status, status_request, 180)
        status_result = (status.get("answer") or {}).get("result") or {}
        trials = status_result.get("trials") or (status_result.get("study") or {}).get("trials") or []
    if not trials:
        problems.append(f"no trials arrived for experiment {experiment_id}: {status_result}")
        return problems, seconds

    explained, explain_seconds, explain_request, _ = turn(
        "agent-experiments",
        f"Explain the result of experiment {experiment_id}, including the best trial and what to do next.",
        "experiment_explain",
        {"experiment_id": experiment_id},
        180)
    seconds += explain_seconds
    explanation = (explained.get("answer") or {}).get("result") or {}
    problems += base_checks(explained, explain_request, 180)
    if explanation.get("kind") != "explanation":
        problems.append(f"the final turn did not explain the experiment: {explanation}")
    if not ((explained.get("answer") or {}).get("reply") or "").strip():
        problems.append("the explanation has no readable reply")
    return problems, seconds


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
    "T11": ("Application Agent builds, confirms, deploys, and answers", t11_application_agent),
    "T12": (
        "Experiments Agent proposes, confirms, observes trials, and explains",
        t12_experiments_agent),
}

def sql_rows(statement):
    answer = api("GET", "workspace_sql", {"customer_id": customer, "workspace_id": workspace,
                                          "statement": statement})
    return ((answer.get("value") or {}).get("rows") or []) if answer.get("ok") else None


def churn_rows():
    """200 accounts, the same every run: the snapshot each row was taken at (`as_of`, the feature
    cutoff), a 90-day churn window after it, tenure, spend and tickets, whether the account churned
    in the window, and a risk score kept apart (ACME-0042 is the riskiest, so T3 has one answer)."""
    for i in range(1, 201):
        tenure, spend, tickets = 1 + (i * 7) % 60, 50 + (i * 37) % 900, (i * 13) % 9
        risk = 0.99 if i == 42 else round(min(0.95, 0.9 - tenure / 80 + tickets / 30), 3)
        yield f"ACME-{i:04d}", f"2026-{1 + i % 6:02d}-{1 + i % 28:02d}", tenure, spend, tickets, risk


def churn_csv():
    lines = ["account,as_of,churn_window_days,tenure_months,monthly_spend,support_tickets,churned"]
    lines += [f"{a},{d},90,{t},{s},{k},{1 if r > 0.6 else 0}" for a, d, t, s, k, r in churn_rows()]
    return "\n".join(lines) + "\n"


def risk_csv():
    return "account,risk\n" + "".join(f"{a},{r}\n" for a, _, _, _, _, r in churn_rows())


def prepare():
    """The data the scenarios read: uploads datasets.ux_review_churn and datasets.ux_review_at_risk
    when they are missing, and answers the SQL that ranks accounts by risk (a run's
    churn_results.at_risk_* table when there is one)."""
    rows = sql_rows("SELECT database, dataset FROM system.datasets")
    if rows is None:
        sys.exit("the workspace cannot list its datasets (workspace_sql); is the workspace ready?")
    names = {(r.get("database"), r.get("dataset")) for r in rows}
    for table, content in (("ux_review_churn", churn_csv()), ("ux_review_at_risk", risk_csv())):
        if ("datasets", table) in names:
            continue
        uploaded = api("POST", "workspace_act", {
            "customer_id": customer, "workspace_id": workspace, "capability": "upload_dataset",
            "input": {"table_name": table, "format": "csv", "content": content, "if_exists": "fail"}})
        if not uploaded.get("ok"):
            sys.exit(f"could not upload datasets.{table}: {json.dumps(uploaded.get('error'))}")
        print(f"prepared: uploaded datasets.{table} (200 rows)", flush=True)
    at_risk = sorted(d for db, d in names if db == "churn_results" and str(d).startswith("at_risk_"))
    table = f"churn_results.{at_risk[-1]}" if at_risk else "datasets.ux_review_at_risk"
    return f"SELECT account, risk FROM {table} ORDER BY risk DESC LIMIT 3"


RANKING = prepare() if not only or set(only) & {"T1", "T2", "T3", "T5", "T6", "T7", "T11", "T12"} else ""

failed = 0
for key, (title, run) in SCENARIOS.items():
    if only and key not in only:
        continue
    problems, seconds = run()
    failed += bool(problems)
    print(f"{key} {'PASS' if not problems else 'FAIL'} {seconds:6.1f}s  {title}"
          + ("" if not problems else "\n      - " + "\n      - ".join(problems)), flush=True)
sys.exit(1 if failed else 0)
