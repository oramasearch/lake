/** Dictation (lake#561, ported from rev-os): dictated text in the box, a recording's length in
 * words, and when lake says it has no speech-to-text. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { MICROPHONE_GONE, NOT_CONFIGURED, appendDictation, isNotSupported, spokenLength } from "../src/lib/dictation.ts";

test("dictation fills an empty box and follows what was typed", () => {
  assert.equal(appendDictation("", "  The export spins. "), "The export spins.");
  assert.equal(appendDictation("On the Projects page:  ", "the export spins."), "On the Projects page: the export spins.");
  assert.equal(appendDictation("Typed", "   "), "Typed");
  assert.equal(appendDictation("   ", "said"), "said");
});

test("a recording's length in words", () => {
  assert.equal(spokenLength(72.4), "1 min 12 s");
  assert.equal(spokenLength(9), "9 s");
  assert.equal(spokenLength(3600), "1 h");
  assert.equal(spokenLength(3725), "1 h 2 min");
  assert.equal(spokenLength(0), "");
  assert.equal(spokenLength(null), "");
  assert.equal(spokenLength(undefined), "");
});

test("not_supported means the lake has no speech-to-text, and nothing else does", () => {
  assert.equal(isNotSupported({ code: "not_supported", message: NOT_CONFIGURED }), true);
  assert.equal(isNotSupported({ code: "invalid_input" }), false);
  assert.equal(isNotSupported(new Error("network")), false);
  assert.equal(isNotSupported(null), false);
  assert.ok(MICROPHONE_GONE.startsWith(NOT_CONFIGURED));
  assert.equal(NOT_CONFIGURED, "Speech-to-text is not configured on this lake.");
});
