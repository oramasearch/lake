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
  ownerKind,
  ownerName,
  pillOwner,
  pillTitle,
  showsOwner,
  workspaceCount,
  type PickerSpace,
} from "../src/lib/workspacePicker.ts";

const ws = (customer: string, ...names: string[]) => names.map((name, i) => ({ id: `${customer}-${i}`, name, customer_id: customer }));
const mine: PickerSpace = { id: "c1", name: "steve-x7f2", label: "Your workspaces", personal: true, kind: "individual", workspaces: ws("c1", "workspace", "Churn study", "Forecasts") };
const acme: PickerSpace = { id: "c2", name: "acme-research", label: "acme-research", personal: false, kind: "organization", workspaces: ws("c2", "Production", "Staging") };
const north: PickerSpace = { id: "c3", name: "Northwind Café", label: "Northwind Café", personal: false, kind: "organization", workspaces: [] };
const viewer = { display_name: "Steve Sperandeo", email: "steve@example.com" };

test("a group's header is the owner as written, with its kind and initial", () => {
  assert.equal(ownerName(mine, viewer), "Steve Sperandeo", "the person, never the generated handle");
  assert.equal(ownerName(mine, { email: "s@example.com" }), "s@example.com");
  assert.equal(ownerName(mine, {}), "You");
  assert.equal(ownerName(acme, viewer), "acme-research", "as written: no upper-casing");
  assert.equal(ownerKind(mine), "Your account");
  assert.equal(ownerKind(acme), "Organization");
  assert.equal(initialOf("Steve Sperandeo"), "S");
  assert.equal(initialOf("  ux-review-2026"), "U");
  assert.equal(initialOf("élan"), "É");
  assert.equal(initialOf("--"), "?");
});

test("the pill names the owner only when there is more than one", () => {
  assert.equal(showsOwner([mine]), false);
  assert.equal(showsOwner([mine, acme]), true);
  assert.equal(pillOwner(mine), "Personal");
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
  const words = filterGroups([mine, acme], viewer, "steve fore");
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
