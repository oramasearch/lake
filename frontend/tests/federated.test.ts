// SPDX-License-Identifier: MIT

/** Federated search (lake#633): the top bar's search and Data → Search share these rules. How a
 * place's answers from several workspaces become one, how hits rank, the excerpt around a match,
 * the top bar's rows and selection, and the "See all results" address. `npm test`. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  PLACE_IDS,
  WORKSPACE_PLACES,
  currentSelection,
  excerptAround,
  hitWhere,
  matchRank,
  mergeWorkspaces,
  moveSelection,
  paletteRows,
  rankHits,
  searchAllUrl,
  selectable,
  stepSelection,
  topHits,
  type Hit,
  type PaletteGroup,
} from "../src/lib/federated.ts";

const hit = (key: string, title: string, extra: Partial<Hit> = {}): Hit => ({ key, title, ...extra });

test("every place Data → Search covers is a place, transcripts among the workspace places", () => {
  assert.deepEqual([...PLACE_IDS], ["repositories", "code", "datasets", "transcripts", "projects", "solutions", "agents", "models", "drives", "docs"]);
  assert.ok(WORKSPACE_PLACES.includes("transcripts"));
  assert.ok(!WORKSPACE_PLACES.includes("docs"));
});

test("a workspace place answers from every workspace, each hit naming its workspace", () => {
  const scopes = [
    { customerId: "c1", workspaceId: "w1", name: "Research" },
    { customerId: "c2", workspaceId: "w2", name: "Sales" },
  ];
  const merged = mergeWorkspaces(scopes, [
    { status: "fulfilled", value: [hit("t1", "Kickoff")] },
    { status: "fulfilled", value: [hit("t1", "ux-review-2026-10-10-a")] },
  ]);
  assert.deepEqual(
    merged.hits.map((h) => [h.key, h.workspace]),
    [
      ["w1:t1", "Research"],
      ["w2:t1", "Sales"],
    ],
    "the same id in two workspaces stays two hits",
  );
  assert.deepEqual(merged.failed, []);
});

test("one workspace is not named; a failed workspace is, and only all failing fails the place", () => {
  const one = mergeWorkspaces([{ customerId: "c", workspaceId: "w", name: "Only" }], [{ status: "fulfilled", value: [hit("a", "A")] }]);
  assert.equal(one.hits[0].workspace, undefined);
  assert.equal(one.hits[0].key, "a");

  const scopes = [
    { customerId: "c", workspaceId: "w1", name: "Up" },
    { customerId: "c", workspaceId: "w2", name: "Down" },
  ];
  const partial = mergeWorkspaces(scopes, [
    { status: "fulfilled", value: [hit("a", "A")] },
    { status: "rejected", reason: new Error("workspace is starting") },
  ]);
  assert.equal(partial.hits.length, 1);
  assert.deepEqual(partial.failed, [{ workspace: "Down", message: "workspace is starting" }]);

  assert.throws(
    () =>
      mergeWorkspaces(scopes, [
        { status: "rejected", reason: new Error("first") },
        { status: "rejected", reason: new Error("second") },
      ]),
    /first/,
  );
  assert.deepEqual(mergeWorkspaces([], []), { hits: [], failed: [] });
});

test("hits rank by how their title matches, keeping the place's order among equals", () => {
  assert.equal(matchRank(hit("a", "Churn"), "churn"), 0);
  assert.equal(matchRank(hit("a", "Churn model"), "churn"), 1);
  assert.equal(matchRank(hit("a", "basic-churn"), "churn"), 2);
  assert.equal(matchRank(hit("a", "Monthly churn"), "churn"), 2);
  assert.equal(matchRank(hit("a", "antichurn"), "churn"), 3);
  assert.equal(matchRank(hit("a", "ux-review-2026-10-10-a", { detail: "churn is up" }), "churn"), 4, "found by its text");
  const ranked = rankHits([hit("1", "text only"), hit("2", "antichurn"), hit("3", "also text"), hit("4", "Churn")], "churn");
  assert.deepEqual(
    ranked.map((h) => h.key),
    ["4", "2", "1", "3"],
  );
  assert.deepEqual(
    topHits(ranked, "churn", 2).map((h) => h.key),
    ["4", "2"],
  );
});

test("a transcript's excerpt shows the text around the match", () => {
  const text = `${"Opening remarks went long. ".repeat(8)}Then Dana said churn is up in the basic plan, so we act. ${"Closing. ".repeat(10)}`;
  const around = excerptAround(text, "CHURN IS UP", 80);
  assert.ok(around.includes("churn is up in the basic plan"), around);
  assert.ok(around.startsWith("…"), around);
  assert.ok(around.endsWith("…"), around);
  assert.ok(around.length <= 84, `${around.length}`);
  assert.equal(excerptAround("short   text\nwith churn", "churn", 80), "short text with churn", "short text: whole, on one line");
  assert.equal(excerptAround("a ".repeat(100), "zzz", 21), `${"a ".repeat(10).trim()}…`, "no match: the start");
  assert.ok(!excerptAround("…lead from the server, churn is up and more text after it to cut here and there", "churn", 40).startsWith("……"));
});

test("a hit's second line names its workspace, then where it lives", () => {
  assert.equal(hitWhere(hit("a", "Kickoff", { workspace: "Sales", detail: "churn is up" }), "churn"), "Sales · churn is up");
  assert.equal(hitWhere(hit("a", "Kickoff", { detail: "datasets" }), "x"), "datasets");
  assert.equal(hitWhere(hit("a", "Kickoff"), "x"), "");
});

test("See all results opens Data → Search with every place on", () => {
  assert.equal(searchAllUrl(" churn is up "), `/search?q=churn+is+up&in=${PLACE_IDS.join("%2C")}`);
});

const goTo = [{ key: "s:/search", label: "Search", hint: "Data" }];
const groups = (over: Partial<Record<string, Partial<PaletteGroup>>> = {}): PaletteGroup[] =>
  [
    { id: "repositories", label: "Repositories", status: "ok", hits: [] },
    { id: "transcripts", label: "Transcripts", status: "ok", hits: [hit("t1", "ux-review-2026-10-10-a", { detail: "churn is up" })] },
    { id: "docs", label: "Docs", status: "loading" },
  ].map((g) => ({ ...g, ...over[g.id] }) as PaletteGroup);

test("the top bar's rows: Go to first, places as they answer, See all last", () => {
  const rows = paletteRows("churn", goTo, groups());
  assert.deepEqual(
    rows.map((r) => r.key),
    ["g:goto", "s:/search", "g:transcripts", "transcripts:t1", "g:docs", "n:docs", "see-all"],
  );
  assert.deepEqual(rows.find((r) => r.key === "n:docs"), { kind: "note", key: "n:docs", text: "Searching…" }, "a slow place says it is searching");
  assert.equal(rows.find((r) => r.key === "g:transcripts")?.kind === "group" && (rows.find((r) => r.key === "g:transcripts") as { count?: number }).count, 1);
});

test("a place that found nothing is left out; a failed place says so in one line", () => {
  const rows = paletteRows("churn", [], groups({ docs: { status: "error", error: "Could not search docs." }, transcripts: { failed: [{ workspace: "Sales", message: "starting" }] } }));
  assert.deepEqual(
    rows.map((r) => r.key),
    ["g:transcripts", "transcripts:t1", "f:transcripts", "g:docs", "n:docs", "see-all"],
  );
  assert.deepEqual(rows.find((r) => r.key === "n:docs"), { kind: "note", key: "n:docs", text: "Could not search docs." });
  assert.deepEqual(rows.find((r) => r.key === "f:transcripts"), { kind: "note", key: "f:transcripts", text: "Could not search Sales." });
  assert.deepEqual(
    paletteRows("churn", [], groups({ docs: { status: "off" } })).map((r) => r.key),
    ["g:transcripts", "transcripts:t1", "see-all"],
    "a place this lake does not offer is left out",
  );
});

test("nothing found anywhere says so once, above See all", () => {
  const rows = paletteRows("zzz", [], groups({ transcripts: { hits: [] }, docs: { status: "ok", hits: [] } }));
  assert.deepEqual(
    rows.map((r) => r.key),
    ["n:none", "see-all"],
  );
});

test("an empty query is Go to alone", () => {
  assert.deepEqual(
    paletteRows("  ", goTo, groups()).map((r) => r.key),
    ["g:goto", "s:/search"],
  );
});

test("each place shows its best few hits", () => {
  const many = Array.from({ length: 9 }, (_, i) => hit(`r${i}`, i === 7 ? "churn" : `repo ${i} churn`));
  const rows = paletteRows("churn", [], [{ id: "repositories", label: "Repositories", status: "ok", hits: many }]);
  const items = selectable(rows).filter((r) => r.group === "repositories");
  assert.equal(items.length, 4);
  assert.equal(items[0].hit?.key, "r7", "the exact name first");
});

test("the keyboard moves across groups by key, and the default never jumps while places answer", () => {
  const items = selectable(paletteRows("churn", goTo, groups()));
  assert.deepEqual(
    items.map((i) => i.key),
    ["s:/search", "transcripts:t1", "see-all"],
  );
  assert.equal(currentSelection(items, null), "s:/search", "the first page of Go to");
  assert.equal(moveSelection(items, "s:/search", 1), "transcripts:t1", "down crosses into a place's group");
  assert.equal(moveSelection(items, "see-all", 1), "see-all", "it stops at the end");
  assert.equal(moveSelection(items, "s:/search", -1), "s:/search");
  assert.equal(moveSelection(items, "gone", 1), "s:/search");
  assert.equal(moveSelection([], null, 1), null);

  const noPages = selectable(paletteRows("churn", [], groups()));
  assert.equal(currentSelection(noPages, null), "see-all", "with no page matching, Enter opens Data → Search");
  assert.equal(currentSelection(noPages, "transcripts:t1"), "transcripts:t1", "a chosen hit stays chosen as others arrive");
  assert.equal(stepSelection(noPages, null, 1), "transcripts:t1", "↓ from the default See all starts at the first result");
  assert.equal(stepSelection(noPages, null, -1), "transcripts:t1", "↑ from See all goes to the result above it");
  assert.equal(stepSelection(items, null, 1), "transcripts:t1", "with a page first, ↓ moves past it");
  assert.equal(stepSelection(items, "see-all", 1), "see-all");
});
