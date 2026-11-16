// SPDX-License-Identifier: MIT

/** Transcripts (lake#561): the words a page shows, default titles, and what Ask Orama is told about
 * one. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { ASK_CONTEXT_CHARS, authorLabel, excerptOf, recordingTitle, sourceLabel, titleFromFile, titleFromText, transcriptAskPage, wordCount } from "../src/lib/transcripts.ts";

test("how a transcript was made, in words", () => {
  assert.equal(sourceLabel("recording"), "Recorded");
  assert.equal(sourceLabel("upload"), "Uploaded");
  assert.equal(sourceLabel("text"), "Pasted");
  assert.equal(sourceLabel(undefined), "Transcript");
  assert.equal(sourceLabel("something-new"), "Transcript");
});

test("a title from a file's name", () => {
  assert.equal(titleFromFile("call_2026-10-09.m4a"), "call 2026-10-09");
  assert.equal(titleFromFile("Weekly sync.vtt"), "Weekly sync");
  assert.equal(titleFromFile("New Recording 12.m4a"), "New Recording 12");
  assert.equal(titleFromFile("archive.tar.gz"), "archive.tar");
  assert.equal(titleFromFile(".m4a"), "Untitled transcript");
});

test("a recording's default title names its day and time (TZ=America/Vancouver)", () => {
  assert.equal(recordingTitle(new Date(2026, 9, 9, 14, 5)), "Recording, Oct 9, 14:05");
  assert.equal(recordingTitle(new Date(2026, 0, 1, 9, 0)), "Recording, Jan 1, 09:00");
});

test("a pasted transcript's title is its first line, shortened", () => {
  assert.equal(titleFromText("\n\n  Kickoff with Acme  \nAda: hello"), "Kickoff with Acme");
  assert.equal(titleFromText(""), "");
  const long = "A".repeat(80);
  const t = titleFromText(long);
  assert.equal(t.length, 60);
  assert.ok(t.endsWith("…"));
});

test("an excerpt, from the server's or from the text", () => {
  assert.equal(excerptOf({ excerpt: "  one\n two  " }), "one two");
  assert.equal(excerptOf({ text: "only the text" }), "only the text");
  assert.equal(excerptOf({}), "");
  const e = excerptOf({ text: "word ".repeat(100) }, 50);
  assert.equal(e.length <= 50, true);
  assert.ok(e.endsWith("…"));
});

test("words in a text", () => {
  assert.equal(wordCount(""), 0);
  assert.equal(wordCount("  one two\nthree  "), 3);
});

test("Ask Orama is told the transcript's id, title and text, cut at the limit", () => {
  const page = transcriptAskPage({ id: "t1", title: "Sync", text: "hello" });
  assert.deepEqual(page, { path: "/transcript", id: "t1", kind: "transcript", title: "Sync", transcript: "hello" });
  for (const value of Object.values(page)) assert.equal(typeof value, "string", "send_ask_message's page takes strings");
  const long = transcriptAskPage({ id: "t2", title: "Long", text: "x".repeat(ASK_CONTEXT_CHARS + 500) });
  assert.ok(long.transcript.startsWith("x".repeat(ASK_CONTEXT_CHARS)));
  assert.match(long.transcript, /only its first 20000 characters are included/);
});

test("a transcript names its author, never by id (lake#579)", () => {
  const row = { created_by: "c344bffe-50b7-4369-8018-022091f13b3c", created_by_name: "Ana Diaz" };
  assert.equal(authorLabel(row, "c344bffe-50b7-4369-8018-022091f13b3c"), "you");
  assert.equal(authorLabel(row, "someone-else"), "Ana Diaz");
  assert.equal(authorLabel(row, undefined), "Ana Diaz");
  assert.equal(authorLabel({ created_by: "c344bffe-50b7-4369-8018-022091f13b3c", created_by_name: null }, "x"), "");
  assert.equal(authorLabel({ created_by: "admin" }, "x"), "");
});
