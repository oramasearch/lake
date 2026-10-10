/**
 * The Design pages' rules (src/pages/design/logic.ts): what lake's renderers and form handlers
 * computed on the server, checked against the server's own behaviour.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  a1Queue,
  actionCounts,
  certificateGroups,
  datasetChoices,
  decisionsRef,
  defaultModelName,
  experimentAllocations,
  allocationShare,
  findMethodology,
  formatG4,
  graphOf,
  groupOpportunities,
  layoutGraph,
  ledgerEntries,
  missingPredicates,
  parseInteger,
  parseNumber,
  plainAgentFailure,
  projectAgentNeedsReply,
  proposedFoundation,
  runsBadge,
  slugText,
  solutionProjectOf,
  solutionState,
  startReturn,
  topologicalOrder,
  withoutProjectAgentChecks,
} from "../src/pages/design/logic.ts";

test("cell numbers print as %.4g", () => {
  assert.equal(formatG4(0.123456), "0.1235");
  assert.equal(formatG4(12.5), "12.5");
  assert.equal(formatG4(12345.6), "1.235e+04");
  assert.equal(formatG4(0.00001234), "1.234e-05");
  assert.equal(formatG4(2.0001), "2");
});

test("a Project Agent failure reads as one plain sentence", () => {
  assert.equal(plainAgentFailure("compile_intent did not succeed: compile_intent FAILED: 403 {...}"), "The Project Agent's compile_intent check did not succeed on your workspace.");
  assert.equal(plainAgentFailure("the table has no label column: {\"x\":1}"), "the table has no label column.");
  assert.equal(plainAgentFailure("{}"), "The Project Agent's check did not succeed on your workspace.");
});

test("a solution name becomes a default project name", () => {
  assert.equal(slugText("Churn & Retention"), "churn-retention");
  assert.equal(slugText("  Next-Best Action! "), "next-best-action");
});

test("result datasets stay out of the pickers", () => {
  const choices = datasetChoices([
    { name: "accounts" },
    { name: "scores", database: "churn_results" },
    { name: "f", database: "forecasts" },
    { name: "x", kind: "result" },
    { name: "orders", database: "sales" },
  ]);
  assert.deepEqual(
    choices.map((c) => c.path),
    ["datasets.accounts", "sales.orders"],
  );
});

test("the Project Agent's check graphs are not pipelines", () => {
  assert.deepEqual(withoutProjectAgentChecks([{ name: "Project Agent check 1" }, { name: "main" }]), [{ name: "main" }]);
});

test("an Opportunity's methodology is its M-n, else the one serving its S-n", () => {
  const registry = [
    { id: "M-1", runnable: true, solutions: ["S-1"] },
    { id: "M-3", runnable: false, solutions: ["S-5"] },
  ];
  assert.equal(findMethodology(registry, "M-3 Segmentation", "")?.id, "M-3");
  assert.equal(findMethodology(registry, "tabular", "S-1 Churn")?.id, "M-1");
  assert.equal(findMethodology(registry, "", ""), null);
  assert.equal(findMethodology(null, "M-1", ""), null);
});

test("numbers parse fully or not at all, as the form handlers did", () => {
  assert.equal(parseNumber("12.5"), 12.5);
  assert.equal(parseNumber(" 3"), 3);
  assert.equal(parseNumber("3 "), null);
  assert.equal(parseNumber("1e3"), 1000);
  assert.equal(parseNumber(""), null);
  assert.equal(parseNumber("12abc"), null);
  assert.equal(parseInteger("2"), 2);
  assert.equal(parseInteger("2.5"), null);
  assert.equal(parseInteger("L2"), null);
});

test("Start your workspace lands on an allowed list page, else Projects", () => {
  assert.equal(startReturn("/train/run"), "/train/runs");
  assert.equal(startReturn("/train/dataset"), "/train/data");
  assert.equal(startReturn("/design/pipeline"), "/design/projects");
  assert.equal(startReturn("/design/solution"), "/design/projects");
  assert.equal(startReturn("/ship/index"), "/ship/indexes");
  assert.equal(startReturn("/ship/apps"), "/design/projects");
});

test("runs badges", () => {
  assert.deepEqual(runsBadge({ status: "sketch", runnable: true }), { label: "In design", tone: "neutral" });
  assert.deepEqual(runsBadge({ runnable: true, partial: true }), { label: "Runs, in part", tone: "success" });
  assert.deepEqual(runsBadge({}), { label: "Not yet", tone: "warning" });
});

test("a solution project is found from its manifest node", () => {
  const found = solutionProjectOf([
    { summary: { name: "plain", id: "g0" }, detail: { nodes: [{ id: "a" }] } },
    {
      summary: { name: "churn", id: "g1" },
      detail: {
        graph_id: "g1",
        nodes: [
          { id: "solution_manifest", attributes: { foundation: { scope: "S-1/x" }, sources: { missing_policy_predicates: [] } } },
          { id: "score", name: "Score", kind: "model", attributes: { solution_stage: 2 } },
        ],
      },
    },
  ]);
  assert.equal(found?.graph, "churn");
  assert.equal(found?.graphId, "g1");
  assert.deepEqual(found?.stages, [{ id: "score", name: "Score", kind: "model", stage: 2 }]);
});

test("the solution summary follows its latest run", () => {
  assert.equal(solutionState([]).lead, "Created.");
  assert.deepEqual(solutionState([{ status: "running", stage: "score" }]), { lead: "Running.", rest: "Reached score.", failed: false });
  assert.deepEqual(solutionState([{ status: "failed", stage: "act", error: "boom" }]), { lead: "Last run failed at act.", rest: "boom", failed: true });
  assert.equal(solutionState([{ status: "succeeded" }]).lead, "Delivering.");
});

test("the decisions table is named only when safe", () => {
  assert.equal(decisionsRef({ outputs: [{ name: "solution decisions", ref: "s1_results.decisions" }] }), "s1_results.decisions");
  assert.equal(decisionsRef({ outputs: [{ name: "solution decisions", ref: "x; DROP" }] }), "");
});

test("the foundation proposal merges the person's inputs and the dataset", () => {
  const task = { answer: { result: { foundation: { predicates: { in_collections: "c" } } } }, context: { delivery: { kind: "webhook" }, dataset: "sales.accounts", solution: "S-1" } };
  const proposal = proposedFoundation(task);
  assert.deepEqual(proposal.delivery, { kind: "webhook" });
  assert.deepEqual(proposal.sources, { source: "sales", table: "accounts" });
  assert.deepEqual(missingPredicates(task, proposal), ["open_complaint", "vulnerable"]);
  assert.deepEqual(proposedFoundation({ answer: {} }), {});
});

test("ledger, counts and the A1 queue", () => {
  const ledger = {
    entries: [
      { kind: "action", cell: "S-1/a", action: "email", subject: "e1", recorded_at: "2026-01-02", score: 0.9 },
      { kind: "action", cell: "S-1/a", action: "no_contact", subject: "e2", decision: "d2", recorded_at: "2026-01-03" },
      { kind: "action", cell: "S-1/a", action: "publish_driver_report", recorded_at: "2026-01-04" },
      { kind: "action", cell: "S-10/a", action: "email", recorded_at: "2026-01-05" },
      { kind: "outcome", cell: "S-1/a", action: "email", recorded_at: "2026-01-06" },
    ],
  };
  const entries = ledgerEntries(ledger, "S-1");
  assert.equal(entries.length, 3);
  assert.deepEqual(actionCounts(entries), { decisions: 2, actions: 1, held: 1 });
  const queue = a1Queue(entries, [
    { id: "d9", autonomy: "A1", status: "pending_approval", action: "call", entity_id: "e9", cell: "S-1/b", score: 0.5 },
    { id: "d2", autonomy: "A2", status: "held" },
  ]);
  assert.deepEqual(
    queue.map((r) => [r.entity, r.proposed, r.governance, r.decisionId]),
    [
      ["e9", "call", "A1 · pending approval", "d9"],
      ["e2", "", "Held · no action delivered", null],
    ],
  );
});

test("experiment allocations fall back to the holdout fraction", () => {
  const allocations = experimentAllocations({ holdout_fraction: 0.2 });
  assert.deepEqual(
    allocations.map((a) => [a.name, allocationShare(a)]),
    [
      ["holdout", "20.0%"],
      ["delivery", "80.0%"],
    ],
  );
});

test("certificate checks group by solution and hide what does not apply", () => {
  const { groups, hidden } = certificateGroups(
    {
      checks: [
        { name: "s1_policy_bound", status: "pass", reason: "ok" },
        { name: "pipeline/non_empty_graph", status: "pass" },
        { name: "b2_other", status: "pass", reason: "no bound for x" },
        { name: "s3_x", status: "fail" },
      ],
    },
    "S-1",
  );
  assert.deepEqual(
    groups.map(([g, c]) => [g, c.length]),
    [
      ["S-1", 1],
      ["S-3", 1],
      ["pipeline", 1],
    ],
  );
  assert.equal(hidden.length, 1);
});

test("Opportunities group by the newest scan", () => {
  const groups = groupOpportunities([
    { id: "1", task_id: "t1", created_at: "2026-01-01", status: "new" },
    { id: "2", task_id: "t2", created_at: "2026-01-02", status: "new" },
    { id: "3", task_id: "t1", created_at: "2026-01-01", status: "not_yet" },
    { id: "4", task_id: "t2", created_at: "2026-01-02", status: "not_yet" },
    { id: "5", task_id: "t1", created_at: "2026-01-01", status: "project" },
  ]);
  assert.equal(groups.latestTask, "t2");
  assert.deepEqual(groups.fresh.map((r) => r.id), ["2"]);
  assert.deepEqual(groups.earlier.map((r) => r.id), ["1"]);
  assert.deepEqual(groups.later.map((r) => r.id), ["4"]);
  assert.deepEqual(groups.made.map((r) => r.id), ["5"]);
});

test("the Run form's model name and the Project Agent's question", () => {
  assert.equal(defaultModelName("Churn Review", "weekly-v2"), "churn_review_weekly_v2");
  assert.equal(projectAgentNeedsReply({ status: "succeeded", answer: { result: { reply: "Which table?" } } }), true);
  assert.equal(projectAgentNeedsReply({ status: "succeeded", answer: { result: { proposal: {} } } }), false);
  assert.equal(projectAgentNeedsReply({ status: "running" }), false);
});

test("the graph is laid out as render_svg lays it out", () => {
  const graph = graphOf({
    name: "p",
    nodes: [
      { id: "m", kind: "model" },
      { id: "c", kind: "connect" },
      { id: "c2", kind: "connect" },
      { id: "z", kind: "weird" },
    ],
    edges: [
      { from: "c", to: "m", kind: "data_flow" },
      { from: "c2", to: "c", kind: "control" },
    ],
  })!;
  assert.deepEqual(topologicalOrder(graph), ["c2", "c", "m", "z"]);
  const layout = layoutGraph(graph);
  assert.deepEqual(layout.columns, ["connect", "model", "other"]);
  assert.deepEqual(layout.position.get("c2"), { x: 18, y: 36 });
  assert.deepEqual(layout.position.get("c"), { x: 18, y: 120 });
  assert.deepEqual(layout.position.get("m"), { x: 218, y: 36 });
  assert.equal(layout.width, 618);
  assert.equal(layout.height, 36 + 2 * 84);
  assert.equal(graphOf("nope"), null);
});
