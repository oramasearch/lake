/** Signers by name (lake#1326): a known identity reads as the account's name and handle, an unknown
 * one as "Unknown signer" and a short key, and a comment's author is found in either shape. `npm
 * test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { authorOf, shortKey, signerLabel, signerText, type People } from "../src/pages/repositories/signers.ts";

const KEY = "taap:1:ed25519:TY44R4SEBNMC23EK45JFTJFFBV5T4QCWV3LCY5OGBHU3WVOXFH3Q";
const people: People = {
  [KEY]: { kind: "user", name: "Ada Lovelace", handle: "ada" },
  "taap:1:ed25519:NONAME": { kind: "user", name: "", handle: "grace" },
};

test("a known signer reads as the name and handle, with the key kept for the hover", () => {
  const label = signerLabel(KEY, people);
  assert.deepEqual(label, { name: "Ada Lovelace", detail: "@ada", known: true, key: KEY });
  assert.equal(signerText(KEY, people), "Ada Lovelace @ada");
  assert.equal(signerLabel(KEY, people, KEY).name, "Ada Lovelace (you)");
});

test("an account with no name reads as its handle", () => {
  assert.deepEqual(signerLabel("taap:1:ed25519:NONAME", people), { name: "@grace", detail: "", known: true, key: "taap:1:ed25519:NONAME" });
});

test("an identity no account acts for reads Unknown signer and a short key", () => {
  const label = signerLabel(KEY, {});
  assert.equal(label.name, "Unknown signer");
  assert.equal(label.detail, "TY44R4SE…FH3Q");
  assert.equal(label.known, false);
  assert.equal(label.key, KEY);
  assert.equal(signerLabel(KEY, undefined).name, "Unknown signer");
  assert.equal(shortKey("taap:1:ed25519:SHORT"), "SHORT");
});

test("a principal that is not a TAAP identity reads as itself", () => {
  assert.deepEqual(signerLabel("app:abc:def", people), { name: "app:abc:def", detail: "", known: false, key: "app:abc:def" });
});

test("a comment's author is a TAAP identifier or liquid's actor object", () => {
  assert.equal(authorOf(KEY), KEY);
  assert.equal(authorOf({ taap_id: KEY, algorithm: "ed25519" }), KEY);
  assert.equal(authorOf(undefined), "");
  assert.equal(authorOf({ name: "x" }), "");
});
