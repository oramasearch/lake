/** Lake on a phone, ported from rev-os (lake#561): recording formats, the keyboard, following new
 * answers, which files a transcript is made from, and which phone. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  ACCEPTED_RECORDINGS,
  AUDIO_ACCEPT,
  MAX_AUDIO_BYTES,
  RECORDING_STOP_BYTES,
  TEXT_ACCEPT,
  clock,
  extensionFor,
  fileProblem,
  keyboardInset,
  kindOf,
  nearBottom,
  onConversationGrew,
  phoneKind,
  pickRecordingFormat,
  recordingFull,
} from "../src/lib/phone.ts";

test("iOS Safari records MP4 (.m4a); Chrome and Firefox WebM/Opus", () => {
  const safari = (t: string) => t === "audio/mp4";
  const chrome = (t: string) => t.startsWith("audio/webm");
  assert.deepEqual(pickRecordingFormat(safari), { mime: "audio/mp4", ext: "m4a" });
  assert.deepEqual(pickRecordingFormat(chrome), { mime: "audio/webm;codecs=opus", ext: "webm" });
  assert.equal(pickRecordingFormat(() => false), null);
  assert.deepEqual(
    pickRecordingFormat((t) => {
      if (t.includes("webm")) throw new Error("unknown");
      return t.startsWith("audio/ogg");
    }),
    { mime: "audio/ogg;codecs=opus", ext: "ogg" },
  );
});

test("the file name follows the type the recorder actually made", () => {
  assert.equal(extensionFor("audio/mp4; codecs=mp4a", null), "m4a");
  assert.equal(extensionFor("audio/webm;codecs=opus", null), "webm");
  assert.equal(extensionFor("audio/mpeg", null), "mp3");
  assert.equal(extensionFor("", { mime: "audio/mp4", ext: "m4a" }), "m4a");
  assert.equal(extensionFor("", null), "webm");
});

test("the keyboard's height is what the visual viewport lost, not the URL bar", () => {
  // iPhone 390×844: the keyboard takes 336 px and Safari scrolls the visual viewport by 0.
  assert.equal(keyboardInset(844, 508, 0), 336);
  // …or scrolls it up: the inset is what lies below it.
  assert.equal(keyboardInset(844, 508, 120), 216);
  // The URL bar collapsing (a few dozen px) is not a keyboard.
  assert.equal(keyboardInset(844, 800, 0), 0);
  // Android resizing the layout viewport itself: nothing to add.
  assert.equal(keyboardInset(508, 508, 0), 0);
});

test("the conversation follows new answers unless the person scrolled up", () => {
  assert.equal(nearBottom(920, 500, 1450), true); // 30 px from the end
  assert.equal(nearBottom(400, 500, 1450), false);
  assert.equal(onConversationGrew(true, false), "follow");
  assert.equal(onConversationGrew(false, false), "chip");
  assert.equal(onConversationGrew(false, true), "follow"); // their own question brings them down
});

test("a recording's clock", () => {
  assert.equal(clock(7.9), "0:07");
  assert.equal(clock(92), "1:32");
  assert.equal(clock(3725), "62:05");
  assert.equal(clock(-1), "0:00");
});

test("a recording stops at its length or before the 25 MB limit", () => {
  assert.equal(recordingFull(0, 59, 60), false);
  assert.equal(recordingFull(0, 60, 60), true);
  assert.equal(recordingFull(RECORDING_STOP_BYTES - 1, 10, 600), false);
  assert.equal(recordingFull(RECORDING_STOP_BYTES, 10, 600), true);
  assert.ok(RECORDING_STOP_BYTES < MAX_AUDIO_BYTES, "the last chunk still fits");
});

test("which files make a transcript, and the limits", () => {
  assert.equal(kindOf("Voice 001.M4A"), "audio");
  assert.equal(kindOf("call.amr"), "audio");
  assert.equal(kindOf("captions.vtt"), "text");
  assert.equal(kindOf("notes.md"), "text");
  assert.equal(kindOf("setup.exe"), null);
  assert.equal(kindOf("README"), null);
  assert.equal(fileProblem({ name: "memo.m4a", size: 1000 }, "audio"), null);
  assert.equal(fileProblem({ name: "call.srt", size: 1000 }, "text"), null);
  assert.match(fileProblem({ name: "memo.m4a", size: 1000 }, "text")!, /isn't a text file/);
  assert.match(fileProblem({ name: "notes.txt", size: 1000 }, "audio")!, new RegExp(ACCEPTED_RECORDINGS.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
  assert.equal(fileProblem({ name: "memo.m4a", size: 0 }, "audio"), "That file is empty.");
  assert.equal(fileProblem({ name: "memo.m4a", size: 26 * 1024 * 1024 }, "audio"), "That file is over the 25 MB limit.");
  assert.equal(fileProblem({ name: "big.txt", size: 6 * 1024 * 1024 }, "text"), "That file is over the 5 MB limit.");
});

test("the pickers offer Files and recordings on a phone, and text where text is wanted", () => {
  const audio = AUDIO_ACCEPT.split(",");
  for (const part of ["audio/*", ".m4a", ".mp3", ".wav", ".aac", ".amr", ".3gp", ".webm"]) assert.ok(audio.includes(part), part);
  assert.ok(!audio.includes(".txt"));
  const text = TEXT_ACCEPT.split(",");
  for (const part of [".txt", ".md", ".vtt", ".srt", "text/plain"]) assert.ok(text.includes(part), part);
  assert.ok(!text.includes("audio/*"));
});

test("which phone the person is using", () => {
  const iphone = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1";
  const android = "Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Mobile Safari/537.36";
  const samsung = "Mozilla/5.0 (Linux; Android 14; SM-S921B) AppleWebKit/537.36 (KHTML, like Gecko) SamsungBrowser/25.0 Chrome/121.0 Mobile Safari/537.36";
  const mac = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15";
  assert.equal(phoneKind(iphone), "iphone");
  assert.equal(phoneKind(android), "android");
  assert.equal(phoneKind(samsung), "android");
  assert.equal(phoneKind(mac, 5), "iphone"); // an iPad asks for the desktop site
  assert.equal(phoneKind(mac, 0), null);
  assert.equal(phoneKind(""), null);
});
