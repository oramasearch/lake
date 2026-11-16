// SPDX-License-Identifier: MIT

/** The top bar's workspace picker (lake#638): owner groups named as written with a kind label and an
 * initial, the pill's owner only when there are several, the filter past eight workspaces, and the
 * one New workspace link. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  FILTER_AFTER,
  filterGroups,
  firstMatch,
  initialOf,
  needsFilter,
  newWorkspacePath,
  orderOwners,
  ownerKind,
  ownerName,
  pillOwner,
  phaseOf,
  pillTitle,
  resolveWorkspace,
  showsOwner,
  workspaceFigures,
  workspaceGroups,
  workspaceCount,
  type PickerSpace,
} from "../src/lib/workspacePicker.ts";

const ws = (customer: string, ...names: string[]) => names.map((name, i) => ({ id: `${customer}-${i}`, name, customer_id: customer }));
const mine: PickerSpace = { id: "c1", name: "ada-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: ws("c1", "workspace", "Churn study", "Forecasts") };
const acme: PickerSpace = { id: "c2", name: "acme-research", label: "acme-research", personal: false, kind: "organization", workspaces: ws("c2", "Production", "Staging") };
const north: PickerSpace = { id: "c3", name: "Northwind Café", label: "Northwind Café", personal: false, kind: "organization", workspaces: [] };
const viewer = { display_name: "Ada Lovelace", email: "ada@example.com" };

test("a group's header is the owner as written, with its kind and initial", () => {
  assert.equal(ownerName(mine, viewer), "Ada Lovelace", "the person, never the generated handle");
  assert.equal(ownerName(mine, { email: "s@example.com" }), "s@example.com");
  assert.equal(ownerName(mine, {}), "You");
  assert.equal(ownerName(acme, viewer), "acme-research", "as written: no upper-casing");
  assert.equal(ownerKind(mine), "Your account");
  assert.equal(ownerKind(acme), "Organization");
  assert.equal(initialOf("Ada Lovelace"), "A");
  assert.equal(initialOf("  ux-review-2026"), "U");
  assert.equal(initialOf("élan"), "É");
  assert.equal(initialOf("--"), "?");
});

test("the pill names the owner only when there is more than one", () => {
  assert.equal(showsOwner([mine]), false);
  assert.equal(showsOwner([mine, acme]), true);
  assert.equal(pillOwner(mine), "Your account");
  assert.equal(pillOwner(acme), "acme-research");
  assert.equal(pillTitle("Churn study", mine, viewer, false), "Churn study");
  assert.equal(pillTitle("Production", acme, viewer, true), "Production — acme-research (Organization)");
});

test("the filter box appears past eight workspaces", () => {
  assert.equal(FILTER_AFTER, 8);
  assert.equal(workspaceCount([mine, acme, north]), 5);
  assert.equal(needsFilter([mine, acme, north]), false);
  const big: PickerSpace = { ...acme, workspaces: ws("c2", "a", "b", "c", "d", "e", "f") };
  assert.equal(needsFilter([mine, big]), true, "nine workspaces");
});

test("filtering keeps matching workspaces and drops owners left empty", () => {
  assert.deepEqual(filterGroups([mine, acme, north], viewer, ""), [mine, acme, north], "no filter: every owner, even an empty one");
  const prod = filterGroups([mine, acme, north], viewer, "PROD");
  assert.deepEqual(prod.map((g) => [g.id, g.workspaces.map((w) => w.name)]), [["c2", ["Production"]]]);
  const byOwner = filterGroups([mine, acme], viewer, "acme");
  assert.deepEqual(byOwner.map((g) => g.workspaces.length), [2], "an owner's name matches its workspaces");
  const words = filterGroups([mine, acme], viewer, "ada fore");
  assert.deepEqual(words.flatMap((g) => g.workspaces.map((w) => w.name)), ["Forecasts"], "every word must match");
  const accents = filterGroups([{ ...north, workspaces: ws("c3", "Café ops") }], viewer, "cafe");
  assert.equal(accents.length, 1, "accents are ignored");
  assert.deepEqual(filterGroups([mine, acme], viewer, "nothing"), []);
  assert.equal(firstMatch(prod), "c2-0");
  assert.equal(firstMatch([]), null);
});

test("New workspace opens Workspaces with its dialog, for the current owner", () => {
  assert.equal(newWorkspacePath("c2"), "/workspaces?customer_id=c2&new=1");
  assert.equal(newWorkspacePath(null), "/workspaces?new=1");
});

test("the person's own account comes first, then each organization in lake's order", () => {
  assert.deepEqual(orderOwners([acme, mine, north]).map((s) => s.id), ["c1", "c2", "c3"]);
});

test("a workspace deleted since it was linked or remembered falls back to one the person can reach", () => {
  const empty: PickerSpace = { ...acme, workspaces: [] };
  // A reported case: the remembered organization's only workspace was deleted.
  const gone = resolveWorkspace([empty, mine], { fromPath: null, remembered: "deleted-ws", suggested: null });
  assert.deepEqual(gone, { id: "c1-0", replace: false, remember: "c1-0" }, "the person's own first workspace, stored in place of the stale one");
  const linked = resolveWorkspace([empty, mine], { fromPath: "deleted-ws", remembered: "deleted-ws", suggested: null });
  assert.deepEqual(linked, { id: "c1-0", replace: true, remember: "c1-0" }, "the URL is rewritten too");
  assert.deepEqual(resolveWorkspace([empty, mine], { fromPath: "c1-2", remembered: "deleted-ws" }), { id: "c1-2", replace: false, remember: null }, "a live URL wins");
  assert.equal(resolveWorkspace([empty, mine], { fromPath: null, remembered: "c1-1", suggested: null }).id, "c1-1", "a live remembered one stays");
  assert.equal(resolveWorkspace([mine, acme], { fromPath: null, remembered: null, suggested: "c2-1" }).id, "c2-1", "lake's suggestion when live");
  const noneOfMine: PickerSpace = { ...mine, workspaces: [] };
  assert.equal(resolveWorkspace([noneOfMine, acme], { fromPath: null, remembered: "x" }).id, "c2-0", "another owner's when the person has none");
  assert.deepEqual(resolveWorkspace([noneOfMine, empty], { fromPath: "x", remembered: "x" }), { id: null, replace: false, remember: null }, "none only when nothing is reachable");
});

test("Workspaces groups every owner in the picker's order with the picker's names and counts", () => {
  const listed = {
    c1: [
      { id: "c1-2", name: "Forecasts", phase: "Ready", storage_gi: 5 },
      { id: "c1-0", name: "workspace", phase: "Ready", storage_gi: 5 },
      { id: "c1-1", name: "Churn study", phase: "Failed", storage_gi: 10 },
      { id: "c1-9", name: "Old", desired: "deleted", phase: "Ready", storage_gi: 5 },
    ],
    c2: [
      { id: "c2-0", name: "Production", phase: "Pending", storage_gi: 20 },
      { id: "c2-1", name: "Staging" },
    ],
    c3: [],
  };
  const groups = workspaceGroups([acme, north, mine], listed);
  assert.deepEqual(
    groups.map((g) => [g.space.id, g.rows?.map((r) => r.name)]),
    [
      ["c1", ["workspace", "Churn study", "Forecasts"]],
      ["c2", ["Production", "Staging"]],
      ["c3", []],
    ],
    "the picker's owners, names and order; a deleting workspace is gone from both",
  );
  const picker = orderOwners([acme, north, mine]);
  assert.deepEqual(groups.map((g) => g.rows?.length), picker.map((s) => s.workspaces.length), "the same counts");
  assert.equal(workspaceGroups([mine], {})[0].rows, null, "an owner not answered yet");
  const extra = workspaceGroups([north], { c3: [{ id: "new", name: "Just made" }] });
  assert.deepEqual(extra[0].rows?.map((r) => r.id), ["new"], "one the viewer read has not seen yet comes last");
  assert.deepEqual(workspaceFigures(groups.flatMap((g) => g.rows ?? [])), { workspaces: 5, ready: 2, failed: 1, storage: 40 }, "figures cover every owner");
  assert.equal(phaseOf({ id: "x", name: "x" }), "pending");
  assert.equal(phaseOf({ id: "x", name: "x", desired: "deleted", phase: "Ready" }), "deleting");
});
