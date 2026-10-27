/** Editing a repository file (lake#1239): Edit only with write access, the exact bytes committed,
 * the stale-file conflict, and the Apps a file redeploys. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
  appsForFile,
  base64ToBytes,
  bytesToBase64,
  commitInput,
  editableFile,
  editRefusal,
  inSourcePath,
  isConflict,
  lineDiff,
  redeployInput,
  rolloutState,
  textBytes,
} from "../src/pages/repositories/edit.ts";

const b64 = (text: string) => bytesToBase64(new TextEncoder().encode(text));
const allowed = { signedIn: true, canWrite: true, checking: false, refName: "main", format: "text", size: 10 };

test("Edit is offered only to someone who may write, on main, for a text file", () => {
  assert.equal(editRefusal(allowed), null);
  assert.match(editRefusal({ ...allowed, canWrite: false }) ?? "", /write access/);
  assert.match(editRefusal({ ...allowed, signedIn: false }) ?? "", /Sign in/);
  assert.match(editRefusal({ ...allowed, checking: true }) ?? "", /Checking/);
  assert.match(editRefusal({ ...allowed, refName: "v1" }) ?? "", /main/);
  assert.match(editRefusal({ ...allowed, format: "image" }) ?? "", /text files/);
  assert.match(editRefusal({ ...allowed, size: 2 * 1024 * 1024 }) ?? "", /1 MiB/);
  for (const format of ["text", "markdown", "csv", "json"]) assert.equal(editRefusal({ ...allowed, format }), null, format);
});

test("the preview page gates the Edit button on that refusal", () => {
  const page = readFileSync(new URL("../src/pages/repositories/DrivePreviewPage.tsx", import.meta.url), "utf8");
  assert.match(page, /disabled=\{refusal !== null\}/);
  assert.match(page, /title=\{refusal \?\?/);
  assert.match(page, /editing = params\.get\("edit"\) === "1" && refusal === null/);
});

test("the editor reads content_base64, never contents (lake#1238)", () => {
  const editor = readFileSync(new URL("../src/pages/repositories/FileEditor.tsx", import.meta.url), "utf8");
  assert.match(editor, /content_base64/);
  assert.doesNotMatch(editor, /\.contents\b/);
});

test("a commit sends the exact bytes it opened, UTF-8, BOM and CRLF included", () => {
  for (const original of ["print('café ✓')\n", "﻿a,b\r\nc,d\r\n", "no newline at end", "", "tab\there\n\n"]) {
    const opened = editableFile(b64(original), original.length);
    assert.ok(opened.ok, original);
    if (!opened.ok) continue;
    const input = commitInput("repo-1", "apps/x/app.py", opened.text, opened.eol, "blob-1", "  Edit app.py ");
    assert.deepEqual([...base64ToBytes(input.operations[0].content)], [...new TextEncoder().encode(original)], JSON.stringify(original));
    assert.equal(input.message, "Edit app.py");
    assert.equal(input.revision, "main");
    assert.equal(input.id, "repo-1");
  }
  // An edit keeps the file's line ending even though a textarea answers LF only.
  assert.equal(new TextDecoder().decode(textBytes("a\nb\n", "\r\n")), "a\r\nb\r\n");
  assert.equal(new TextDecoder().decode(textBytes("a\r\nb", "\n")), "a\nb");
});

test("a commit names the blob it opened, so a changed file is a conflict, never an overwrite", () => {
  const input = commitInput("r", "a.txt", "x", "\n", "abc123", "m");
  assert.deepEqual(input.operations, [{ type: "file", path: "a.txt", content: b64("x"), expected_blob: "abc123" }]);
  assert.ok(isConflict({ code: "conflict" }));
  assert.ok(!isConflict({ code: "unauthorized" }));
  assert.ok(!isConflict(null));
  const editor = readFileSync(new URL("../src/pages/repositories/FileEditor.tsx", import.meta.url), "utf8");
  // While a conflict is shown, neither commit button can be pressed.
  assert.equal((editor.match(/disabled=\{!changed \|\| !!conflict \|\| busy !== null\}/g) ?? []).length, 2);
});

test("files the editor cannot keep exact are not opened", () => {
  assert.match((editableFile(bytesToBase64(new Uint8Array([0xff, 0xfe, 0x41])), 3) as { reason: string }).reason, /not UTF-8/);
  assert.match((editableFile(bytesToBase64(new Uint8Array([0x41, 0, 0x42])), 3) as { reason: string }).reason, /binary/);
  assert.match((editableFile(b64("a\r\nb\nc"), 6) as { reason: string }).reason, /mixes line endings/);
  assert.match((editableFile(b64("a\rb"), 3) as { reason: string }).reason, /mixes line endings/);
  assert.match((editableFile(undefined, 0) as { reason: string }).reason, /bytes/);
});

test("the Apps a file belongs to, and their redeploy", () => {
  const apps = [
    { id: "1", name: "dash", mode: "runtime", source: { repository: "r", path: "apps/dash", entrypoint: "app.py" } },
    { id: "2", name: "root", mode: "runtime", source: { repository: "r", path: "." } },
    { id: "3", name: "other", mode: "runtime", source: { repository: "s", path: "" } },
    { id: "4", name: "image", mode: "image", source: null },
    { id: "5", name: "dashboard", mode: "runtime", source: { repository: "r", path: "apps/dash2" } },
  ];
  assert.deepEqual(appsForFile(apps, "r", "apps/dash/app.py").map((app) => app.name), ["dash", "root"]);
  assert.deepEqual(appsForFile(apps, "r", "README.md").map((app) => app.name), ["root"]);
  assert.ok(inSourcePath("./apps/", "apps/x"));
  assert.ok(!inSourcePath("apps", "appsx/y"));
  assert.deepEqual(redeployInput(apps[0], "c", "w", "rev9"), {
    customer_id: "c",
    workspace_id: "w",
    app_id: "1",
    source: { repository: "r", revision: "rev9", path: "apps/dash", entrypoint: "app.py" },
  });
});

test("a rollout is followed until the deployed generation is Ready or fails", () => {
  assert.equal(rolloutState(undefined, 3), "rolling");
  assert.equal(rolloutState({ observed_generation: 2, phase: "Running", ready: true }, 3), "rolling");
  assert.equal(rolloutState({ observed_generation: 3, phase: "Deploying", ready: false }, 3), "rolling");
  assert.equal(rolloutState({ observed_generation: 3, phase: "Running", ready: true }, 3), "ready");
  assert.equal(rolloutState({ observed_generation: 3, phase: "Failed" }, 3), "failed");
  assert.equal(rolloutState({ observed_generation: 4, phase: "Degraded" }, 3), "failed");
});

test("the conflict diff", () => {
  assert.deepEqual(lineDiff("a\nb\nc", "a\nx\nc"), [
    { kind: "same", text: "a" },
    { kind: "removed", text: "b" },
    { kind: "added", text: "x" },
    { kind: "same", text: "c" },
  ]);
  assert.deepEqual(lineDiff("", ""), [{ kind: "same", text: "" }]);
});
