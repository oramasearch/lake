/** Usage over time (lake#564): the dashboard's windows, units, changes, readings of several accounts
 * added up, the stacked chart, the CSV, limits in a person's units, the links other pages make, and
 * Home's workspace selection that Usage shares. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  billingNote,
  breakdownName,
  change,
  firstLimitDimension,
  formatValue,
  fromLimit,
  limitDimensions,
  limitNote,
  limitTone,
  mergeReadings,
  rangeOf,
  stackSeries,
  csvCell,
  toCsv,
  toLimit,
  usageLink,
  usageOf,
  windows,
  type Dimension,
  type Reading,
} from "../src/pages/usage/usage.ts";
import { chosenWorkspaceIds, selectionToKeep, toggleWorkspace } from "../src/lib/workspaceSelection.ts";

const now = new Date("2026-10-10T15:42:00Z");

test("each range is whole UTC periods, compared with the one before", () => {
  assert.deepEqual(windows("month", now), {
    current: { from: "2026-10-01T00:00:00Z", to: "2026-11-01T00:00:00Z", granularity: "day" },
    previous: { from: "2026-09-01T00:00:00Z", to: "2026-10-01T00:00:00Z", granularity: "day" },
  });
  assert.deepEqual(windows("24h", now).current, { from: "2026-10-09T16:00:00Z", to: "2026-10-10T16:00:00Z", granularity: "hour" });
  assert.equal(windows("24h", now).previous.to, "2026-10-09T16:00:00Z");
  assert.deepEqual(windows("7d", now).current, { from: "2026-10-04T00:00:00Z", to: "2026-10-11T00:00:00Z", granularity: "day" });
  assert.deepEqual(windows("12m", now).current, { from: "2025-11-01T00:00:00Z", to: "2026-11-01T00:00:00Z", granularity: "month" });
  assert.equal(windows("12m", now).previous.from, "2024-11-01T00:00:00Z");
  // January's month before is December of the year before.
  assert.equal(windows("month", new Date("2027-01-05T00:00:00Z")).previous.from, "2026-12-01T00:00:00Z");
  assert.equal(rangeOf("7d"), "7d");
  assert.equal(rangeOf("forever"), "month");
  assert.equal(rangeOf(null), "month");
});

test("each unit reads in a person's words", () => {
  assert.equal(formatValue(1536, "bytes"), "1.5 KiB");
  assert.equal(formatValue(90, "seconds"), "2 min");
  assert.equal(formatValue(5400, "seconds"), "1.5 h");
  assert.equal(formatValue(7200, "instance_seconds"), "2 instance-h");
  assert.equal(formatValue(36000 * 5, "gib_seconds"), "50 GiB-h");
  // lake#643: CPU in mCPU-seconds, RAM and GPU memory in MiB-seconds, GPU in GPU-seconds.
  assert.equal(formatValue(500 * 7200, "mcpu_seconds"), "1 vCPU-h");
  assert.equal(formatValue(1024 * 3600 * 24, "mib_seconds"), "24 GiB-h");
  assert.equal(formatValue(5400, "gpu_seconds"), "1.5 GPU-h");
  assert.equal(formatValue(12_500, "tokens"), "12.5k tokens");
  assert.equal(formatValue(2_400_000, "calls"), "2.4M calls");
  assert.equal(formatValue(null, "calls"), "—");
});

test("the change against the last period", () => {
  assert.deepEqual(change(150, 100), { text: "+50%", direction: "up" });
  assert.deepEqual(change(95, 100), { text: "-5.0%", direction: "down" });
  assert.deepEqual(change(100, 100), { text: "no change", direction: "flat" });
  assert.deepEqual(change(5, 0), { text: "new this period", direction: "new" });
  assert.deepEqual(change(0, 0), { text: "no change", direction: "flat" });
});

const dim = (over: Partial<Dimension>): Dimension => ({
  dimension: "calls",
  title: "API and MCP Calls",
  unit: "calls",
  aggregation: "sum",
  total: 0,
  points: [],
  by_workspace: [],
  by_kind: [],
  ...over,
});
const reading = (customer: string, dimensions: Dimension[], workspaces: string[]): Reading => ({
  customer_id: customer,
  from: "2026-10-01T00:00:00Z",
  to: "2026-10-03T00:00:00Z",
  granularity: "day",
  periods: ["2026-10-01T00:00:00Z", "2026-10-02T00:00:00Z"],
  dimensions,
  workspaces: workspaces.map((id) => ({ workspace_id: id, name: id, measured: true, message: "" })),
  not_measured: ["CPU and memory"],
});

test("two accounts' readings add up dimension by dimension and breakdown by breakdown", () => {
  const a = reading("a", [dim({ total: 5, points: [2, 3], by_workspace: [{ key: "w1", total: 5, points: [2, 3] }], by_kind: [{ key: "api", total: 5, points: [2, 3] }] })], ["w1"]);
  const b = reading("b", [dim({ total: 4, points: [null, 4], by_workspace: [{ key: "w2", total: 4, points: [null, 4] }], by_kind: [{ key: "api", total: 1, points: [null, 1] }, { key: "mcp", total: 3, points: [null, 3] }] })], ["w2"]);
  const merged = mergeReadings([a, b])!;
  const calls = merged.dimensions[0];
  assert.equal(calls.total, 9);
  assert.deepEqual(calls.points, [2, 7]);
  assert.deepEqual(calls.by_workspace.map((w) => w.key), ["w1", "w2"]);
  assert.deepEqual(calls.by_kind.map((k) => [k.key, k.total]), [["api", 6], ["mcp", 3]]);
  assert.equal(merged.workspaces.length, 2);
  assert.deepEqual(merged.not_measured, ["CPU and memory"]);
  assert.equal(merged.customer_id, undefined);
  assert.equal(mergeReadings([a])!.customer_id, "a");
  assert.equal(mergeReadings([]), null);
});

test("the chart stacks the largest workspaces and puts the rest in Other", () => {
  const d = dim({
    points: [10, 20],
    by_workspace: [
      { key: "w1", total: 12, points: [4, 8] },
      { key: "w2", total: 9, points: [3, 6] },
      { key: "w3", total: 6, points: [2, 4] },
      { key: "w4", total: 3, points: [1, 2] },
    ],
  });
  const series = stackSeries(d, "workspace", { w1: "Home" }, 2);
  assert.deepEqual(series.labels, ["Home", "w2", "Other"]);
  assert.deepEqual(series.values, [[4, 8], [3, 6], [3, 6]]);
  assert.deepEqual(series.stacked, [[4, 8], [7, 14], [10, 20]]);
  // A reading's period without data stays empty, and kinds read in words.
  const storage = dim({ dimension: "storage", unit: "bytes", aggregation: "last", points: [null, 5], by_kind: [{ key: "workspace_data", total: 5, points: [null, 5] }] });
  const kinds = stackSeries(storage, "kind");
  assert.deepEqual(kinds.labels, ["Workspace data"]);
  assert.deepEqual(kinds.stacked, [[null, 5]]);
});

test("the CSV has one row per dimension, breakdown and period in the dimension's own unit", () => {
  const r = reading("a", [dim({ total: 5, points: [2, 3], by_workspace: [{ key: "w1", total: 5, points: [2, 3] }], by_kind: [] })], ["w1"]);
  const csv = toCsv(r, { w1: "Home, the first" }).trim().split("\n");
  assert.equal(csv[0], "dimension,unit,breakdown,name,period,value");
  assert.equal(csv[1], "calls,calls,total,,2026-10-01T00:00:00Z,2");
  assert.equal(csv[3], 'calls,calls,workspace,"Home, the first",2026-10-01T00:00:00Z,2');
  assert.equal(csv.length, 5);
});

test("a limit is typed in GiB, hours or a count and kept in the dimension's unit", () => {
  assert.equal(toLimit("5", "bytes"), 5 * 1024 ** 3);
  assert.equal(toLimit("1.5", "seconds"), 5400);
  assert.equal(toLimit("100", "instance_seconds"), 360_000);
  assert.equal(toLimit("2", "mcpu_seconds"), 7_200_000, "vCPU-hours");
  assert.equal(toLimit("1", "mib_seconds"), 3_686_400, "GiB-hours");
  assert.equal(fromLimit(7_200_000, "mcpu_seconds"), "2");
  assert.equal(toLimit("1000", "calls"), 1000);
  assert.equal(toLimit("ten", "calls"), null);
  assert.equal(toLimit("-1", "calls"), null);
  assert.equal(fromLimit(5 * 1024 ** 3, "bytes"), "5");
  assert.equal(fromLimit(5400, "seconds"), "1.5");
  assert.equal(limitTone("over"), "bad");
  assert.equal(limitTone("warning"), "warn");
  assert.equal(limitTone("ok"), undefined);
});

test("other pages link to Usage for their workspace and dimension", () => {
  assert.equal(usageLink(), "/usage");
  assert.equal(usageLink({ workspaces: ["a", "b"], dimension: "instances" }), "/usage?ws=a,b&dimension=instances");
  assert.equal(usageLink({ customerId: "c-1", range: "12m" }), "/usage?customer_id=c-1&range=12m");
  assert.equal(usageLink({ range: "month" }), "/usage");
});

test("one thing's usage is its rows of the breakdown", () => {
  const r = reading("a", [dim({ dimension: "instances", unit: "instance_seconds", by_workspace: [], by_kind: [], by_object: [{ key: "svc-1", total: 7200, points: [7200, 0] }, { key: "svc-2", total: 10, points: [10, 0] }] }), dim({ dimension: "model_tokens", unit: "tokens", by_agent: [{ key: "Churn Watcher", total: 40, points: [] }] })], ["w1"]);
  assert.deepEqual(usageOf(r, [{ dimension: "instances", breakdown: "by_object", keys: ["svc-1"] }]), [{ dimension: "instances", title: "API and MCP Calls", unit: "instance_seconds", value: 7200 }]);
  assert.equal(usageOf(r, [{ dimension: "model_tokens", breakdown: "by_agent", keys: ["Churn Watcher", "agent-1"] }])[0].value, 40);
  assert.equal(usageOf(r, [{ dimension: "volumes", breakdown: "by_object", keys: ["svc-1"] }])[0].value, 0);
  assert.deepEqual(usageOf(null, [{ dimension: "calls", breakdown: "by_object", keys: ["x"] }]), []);
});

test("the workspace selection: the URL, then what was remembered, then every workspace", () => {
  const known = ["a", "b", "c"];
  assert.deepEqual(chosenWorkspaceIds("a,zz,c", ["b"], known), ["a", "c"]);
  assert.deepEqual(chosenWorkspaceIds(null, ["b", 7, "gone"], known), ["b"]);
  assert.deepEqual(chosenWorkspaceIds(null, "junk", known), []);
  assert.deepEqual(selectionToKeep(["a", "b", "c"], known), []);
  assert.deepEqual(selectionToKeep(["a"], known), ["a"]);
  assert.deepEqual(toggleWorkspace([], true, "b"), ["b"]);
  assert.deepEqual(toggleWorkspace(["a", "b"], false, "b"), ["a"]);
  assert.deepEqual(toggleWorkspace(["a"], false, "a"), []);
});

test("build, run and instance time read as detail that is not billed (lake#658)", () => {
  assert.equal(billingNote({ billed: false }), "Detail, not billed");
  assert.equal(billingNote({ billed: true }), null);
  assert.equal(billingNote({}), null, "a reading from before lake#658 says nothing");
  assert.equal(
    billingNote({ billed: false, dimension: "engine_storage" }),
    "Detail, billed as the engine's volume",
    "what an engine holds is paid for by its volume (lake#663)",
  );
  assert.equal(breakdownName({ key: "models", total: 0, points: [] }), "Models");
  assert.equal(breakdownName({ key: "indexes", total: 0, points: [] }), "Indexes");
  assert.equal(breakdownName({ key: "dictation", total: 0, points: [] }), "Dictation");
});

test("a limit says what it does at its line: storage and model tokens refuse, the rest warn", () => {
  assert.equal(limitNote({ dimension: "storage", enforced: true, warn_percent: 80 }), "Enforced on new repository writes");
  assert.equal(limitNote({ dimension: "model_tokens", enforced: true, warn_percent: 80 }), "This month; agents' model calls are refused at the limit");
  assert.equal(limitNote({ dimension: "cpu", enforced: false, warn_percent: 75 }), "This month; warns from 75%");
});

test("only the operator is offered the storage limit, and the form never starts on it (lake#976)", () => {
  const dimensions = [{ dimension: "storage" }, { dimension: "api_calls" }, { dimension: "model_tokens" }];
  // An account administrator saving storage is refused, so the form does not offer it.
  assert.deepEqual(
    limitDimensions(dimensions, false).map((d) => d.dimension),
    ["api_calls", "model_tokens"],
  );
  assert.equal(firstLimitDimension(dimensions, false), "api_calls");
  // The operator (the instance administrator) sets the storage quota, but chooses it on purpose.
  assert.deepEqual(
    limitDimensions(dimensions, true).map((d) => d.dimension),
    ["storage", "api_calls", "model_tokens"],
  );
  assert.equal(firstLimitDimension(dimensions, true), "api_calls");
  assert.equal(firstLimitDimension([{ dimension: "storage" }], true), "storage");
  assert.equal(firstLimitDimension([{ dimension: "storage" }], false), "");
  assert.equal(firstLimitDimension([], false), "");
});

test("a name a person chose stays literal text in the CSV (LSR-201, lake#1007)", () => {
  assert.equal(csvCell("=HYPERLINK(\"https://example.test\")"), `"'=HYPERLINK(""https://example.test"")"`);
  assert.equal(csvCell("+1"), "'+1");
  assert.equal(csvCell("-2"), "'-2");
  assert.equal(csvCell("@SUM(A1)"), "'@SUM(A1)");
  assert.equal(csvCell("Home"), "Home");
  assert.equal(csvCell(-3), "-3", "a number is written as it is");
});
