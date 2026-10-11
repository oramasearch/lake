/** Drives (lake#561): paths, names, the upload and move plans, range selection, upload chunks and
 * what a shortcut opens. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { ancestors, chunkBySize, chunkRanges, cleanPath, driveSlug, expandTo, isWithin, joinPath, nameOf, parentOf, planMoves, planUpload, rangeSelect, sortEntries, validName } from "../src/pages/drives/paths.ts";
import { cleanTarget, nameAfterTarget, selectsOnFocus, storageLabel, targetHref, targetProblem } from "../src/pages/drives/targets.ts";

test("drive paths", () => {
  assert.equal(cleanPath("/a//b/./c/"), "a/b/c");
  assert.equal(joinPath("", "x"), "x");
  assert.equal(joinPath("a/b", "/c/"), "a/b/c");
  assert.equal(parentOf("a/b/c"), "a/b");
  assert.equal(parentOf("a"), "");
  assert.equal(nameOf("a/b/c.txt"), "c.txt");
  assert.deepEqual(ancestors("a/b"), ["", "a", "a/b"]);
  assert.ok(isWithin("a/b", "a"));
  assert.ok(!isWithin("ab", "a"));
  assert.deepEqual([...expandTo(new Set(["x"]), "a/b")].sort(), ["", "a", "a/b", "x"]);
});

test("a name lake accepts", () => {
  for (const good of ["Q3 report (final).pdf", "été", "a".repeat(255)]) assert.ok(validName(good), good);
  for (const bad of ["", ".", "..", ".lake-folder", "a/b", "a\\b", "tab\there", "é".repeat(128)]) assert.ok(!validName(bad), bad);
  assert.equal(driveSlug("Team Files — Q3!"), "team-files-q3");
});

test("folders first, then by the key", () => {
  const rows = [
    { path: "b.txt", kind: "file" as const, size: 5, modifiedAt: "2026-10-02" },
    { path: "z", kind: "folder" as const, size: 0 },
    { path: "a.txt", kind: "file" as const, size: 9, modifiedAt: "2026-10-01" },
    { path: "link", kind: "shortcut" as const, size: 0 },
  ];
  assert.deepEqual(sortEntries(rows, "name", "asc").map((r) => r.path), ["z", "a.txt", "b.txt", "link"]);
  assert.deepEqual(sortEntries(rows, "size", "desc").map((r) => r.path), ["z", "a.txt", "b.txt", "link"]);
});

test("shift-click selects a range", () => {
  const order = ["a", "b", "c", "d"];
  assert.deepEqual(rangeSelect(order, "b", "d"), ["b", "c", "d"]);
  assert.deepEqual(rangeSelect(order, "d", "a"), ["a", "b", "c", "d"]);
  assert.deepEqual(rangeSelect(order, null, "c"), ["c"]);
  assert.deepEqual(rangeSelect(order, "gone", "c"), ["c"]);
});

test("a folder dropped whole is uploaded with its folders first", () => {
  const plan = planUpload("Docs", [
    { relativePath: "Trip/Day 1/a.jpg", size: 3, file: "a" },
    { relativePath: "Trip/b.jpg", size: 2, file: "b" },
    { relativePath: "top.txt", size: 1, file: "t" },
    { relativePath: "bad/../x", size: 1, file: "x" },
  ]);
  assert.deepEqual(plan.folders, ["Docs/Trip", "Docs/Trip/Day 1"]);
  assert.deepEqual(plan.batches.map((b) => b.folder), ["Docs", "Docs/Trip", "Docs/Trip/Day 1"]);
  assert.deepEqual(plan.refused, ["bad/../x"]);
});

test("moving items onto a folder", () => {
  const { moves, skipped } = planMoves(["a/x", "a/x/y", "b", "c/z"], "c");
  assert.deepEqual(moves, [
    { from: "a/x", to: "c/x" },
    { from: "b", to: "c/b" },
  ]);
  assert.deepEqual(skipped.map((s) => s.path), ["c/z"]);
  assert.equal(planMoves(["c"], "c/d").skipped[0].reason, "a folder cannot move into itself");
});

test("uploads in requests that fit, and large files in whole blocks", () => {
  const { chunks, tooLarge } = chunkBySize(
    [
      { name: "a", size: 30 },
      { name: "b", size: 30 },
      { name: "huge", size: 1000 },
    ],
    400,
  );
  assert.deepEqual(chunks.map((c) => c.map((f) => f.name)), [["a"], ["b"]]);
  assert.deepEqual(tooLarge.map((f) => f.name), ["huge"]);
  const MiB = 1024 * 1024;
  assert.deepEqual(chunkRanges(20 * MiB + 5, MiB, 8 * MiB), [
    { offset: 0, length: 8 * MiB },
    { offset: 8 * MiB, length: 8 * MiB },
    { offset: 16 * MiB, length: 4 * MiB + 5 },
  ]);
  assert.deepEqual(chunkRanges(0, MiB, 8 * MiB), []);
  assert.deepEqual(chunkRanges(10, MiB, 100), [{ offset: 0, length: 10 }], "never less than one block a chunk");
});

test("what a shortcut opens", () => {
  assert.equal(targetHref({ kind: "repository", repository_id: "r1" })?.href, "/repository?id=r1");
  assert.equal(targetHref({ kind: "repository_path", repository_id: "r1", path: "docs/q3.pdf" })?.href, "/drive/preview?repository_id=r1&ref=main&path=docs%2Fq3.pdf");
  assert.equal(targetHref({ kind: "repository_path", repository_id: "r1", path: "docs" })?.href, "/drive?repository_id=r1&ref=main&path=docs");
  assert.deepEqual(targetHref({ kind: "drive_entry", drive_id: "d", entry_id: "e" }), { href: "/drives/drive?id=d&focus=e", external: false, workspace: true });
  assert.equal(targetHref({ kind: "dataset", id: "sales/orders" })?.href, "/train/dataset?name=orders&database=sales");
  assert.equal(targetHref({ kind: "transcript", id: "t" })?.href, "/transcript?id=t");
  assert.equal(targetHref({ kind: "project", name: "churn" })?.href, "/design/project?name=churn");
  assert.equal(targetHref({ kind: "model", id: "m 1" })?.href, "/models/m%201");
  assert.deepEqual(targetHref({ kind: "url", url: "https://x.test/" }), { href: "https://x.test/", external: true, workspace: false });
  assert.equal(targetHref({ kind: "url", url: "javascript:alert(1)" }), null);
  assert.equal(storageLabel("files"), "Workspace files");
});

test("a shortcut's target is checked as lake checks it", () => {
  assert.equal(targetProblem({ kind: "repository" }), "Choose a repository.");
  assert.equal(targetProblem({ kind: "repository_path", repository_id: "r" }), "Name the path in the repository.");
  assert.equal(targetProblem({ kind: "url", url: "ftp://x" }), "A web address starts with http:// or https://.");
  assert.equal(targetProblem({ kind: "url", url: "https://x.test" }), null);
  assert.deepEqual(cleanTarget({ kind: "repository_path", repository_id: " r ", path: "/a/b/", id: "stray", url: "x" }), { kind: "repository_path", repository_id: "r", path: "a/b" });
});

test("New shortcut: a suggested name is replaced by typing, a typed one is kept (lake#580)", () => {
  assert.equal(nameAfterTarget("", false, "app-e2e-notes"), "app-e2e-notes");
  assert.equal(nameAfterTarget("app-e2e-notes", false, "other"), "other", "a new target suggests again");
  assert.equal(nameAfterTarget("notes app source", true, "app-e2e-notes"), "notes app source", "what was typed stays");
  assert.equal(selectsOnFocus("app-e2e-notes", false), true, "focus selects the suggestion, so typing replaces it");
  assert.equal(selectsOnFocus("notes app source", true), false);
  assert.equal(selectsOnFocus("", false), false);
});
