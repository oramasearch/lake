// SPDX-License-Identifier: MIT

/** Repository issues (lake#598): the views, labels and their colours, people's names, the smallest
 * update an edit sends and why a person cannot write. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { issueChanges, issueHref, issueMatches, issueView, knownLabels, labelTone, parseLabels, personName, toggle, writeReason, type IssueRecord, type Person } from "../src/pages/repositories/issues.ts";

const issue = (id: string, value: IssueRecord["value"]): IssueRecord => ({ id, created_at: "2026-10-10T00:00:00Z", updated_at: "2026-10-10T00:00:00Z", value });

test("the view: open unless the address says closed or all", () => {
  assert.equal(issueView(null), "open");
  assert.equal(issueView(""), "open");
  assert.equal(issueView("closed"), "closed");
  assert.equal(issueView("all"), "all");
  assert.equal(issueView("anything"), "open");
});

test("which issues a view shows", () => {
  const a = issue("a", { title: "Import fails", labels: ["bug"], assignees: ["taap:ana"] });
  const b = issue("b", { title: "Docs", state: "closed", labels: ["docs"] });
  const all = [a, b];
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "open" })).map((i) => i.id), ["a"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "closed" })).map((i) => i.id), ["b"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "all" })).map((i) => i.id), ["a", "b"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "all", label: "docs" })).map((i) => i.id), ["b"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "all", assignee: "taap:ana" })).map((i) => i.id), ["a"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "all", search: "  IMPORT " })).map((i) => i.id), ["a"]);
  assert.deepEqual(all.filter((i) => issueMatches(i, { view: "all", search: "b" })).map((i) => i.id), ["b"], "an id matches too");
});

test("labels as typed, toggled and known", () => {
  assert.deepEqual(parseLabels("bug, needs triage,  bug ,, "), ["bug", "needs triage"]);
  assert.deepEqual(parseLabels(""), []);
  assert.deepEqual(toggle(["bug"], "docs"), ["bug", "docs"]);
  assert.deepEqual(toggle(["bug", "docs"], "bug"), ["docs"]);
  assert.deepEqual(knownLabels([issue("a", { labels: ["ui", "bug"] }), issue("b", { labels: ["bug"] }), issue("c", {})]), ["bug", "ui"]);
});

test("a label always has the same colour, and trouble reads red", () => {
  assert.equal(labelTone("bug"), "bad");
  assert.equal(labelTone(" Security "), "bad");
  assert.equal(labelTone("docs"), labelTone("docs"));
  assert.equal(labelTone("Docs"), labelTone("docs"));
  for (const name of ["docs", "ui", "import", "enhancement", "question"]) assert.notEqual(labelTone(name), "bad", name);
});

test("who a TAAP identity is", () => {
  const people: Person[] = [
    { taap_id: "taap:ana", kind: "user", display_name: "Ana Lima", email: "ana@example.com", handle: "ana" },
    { taap_id: "taap:bo", kind: "user", display_name: "", email: "bo@example.com", handle: "bo" },
    { taap_id: "taap:cy", kind: "user", display_name: "", email: "cy@example.com", handle: "" },
  ];
  assert.equal(personName("taap:ana", people), "Ana Lima");
  assert.equal(personName("taap:bo", people), "@bo");
  assert.equal(personName("taap:cy", people), "cy@example.com");
  assert.equal(personName("octocat", people), "octocat", "a GitHub login reads as itself");
  assert.equal(personName("taap:ana", people, "taap:ana"), "Ana Lima (you)");
  assert.equal(personName("taap:zed", [], "taap:zed"), "you");
  assert.equal(personName(undefined, people), "");
  assert.equal(personName("taap:1:ed25519:abcdefghijklmnop", []), "taap:1:ed255…jklmnop", "an unknown identity is abbreviated");
});

test("an edit sends only what changed", () => {
  const before = { title: "Import fails", body: "Steps", labels: ["bug"], assignees: ["taap:ana"] };
  assert.deepEqual(issueChanges(before, { title: " Import fails ", body: "Steps", labels: ["bug"], assignees: ["taap:ana"] }), {});
  assert.deepEqual(issueChanges(before, { title: "Import fails on empty files" }), { title: "Import fails on empty files" });
  assert.deepEqual(issueChanges(before, { body: "Steps\n\n1. Upload" }), { body: "Steps\n\n1. Upload" });
  assert.deepEqual(issueChanges(before, { labels: ["bug", "import"] }), { labels: ["bug", "import"] });
  assert.deepEqual(issueChanges(before, { assignees: [] }), { assignees: [] });
  assert.deepEqual(issueChanges({}, { labels: [] }), {});
});

test("an issue's address and why a person cannot write", () => {
  assert.equal(issueHref("r 1", "i/2"), "/issue?repository_id=r%201&id=i%2F2");
  assert.match(writeReason(false, false, false)!, /^Sign in/);
  assert.match(writeReason(true, false, false)!, /write access/);
  assert.equal(writeReason(true, true, false), null);
  assert.ok(writeReason(true, false, true));
});
