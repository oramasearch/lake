/** Account → Identity (lake#762): the managed key and using your own identity. `npm test`. */
import assert from "node:assert/strict";
import {readdirSync, readFileSync, statSync} from "node:fs";
import {join} from "node:path";
import {test} from "node:test";

import {keySummary, looksSigned, signCommand, signedOutput} from "../src/lib/identity.ts";

test("the summary says managed key or own key", () => {
    assert.match(keySummary({
                     key : "managed",
                     linked : null,
                     managed_key : {public_key : "k", held : true, destroyed_at : null}
                 }),
                 /^Managed key/);
    assert.match(
        keySummary({
            key : "own",
            linked : {own_taap_id : "taap:1:ed25519:X", own_document_hash : "h", linked_at : "t"},
            managed_key : null
        }),
        /^Your own key: the managed key was replaced/,
    );
    assert.match(keySummary({key : "own", linked : null, managed_key : null}),
                 /registered it when you signed up/);
});

test("the sign command is liquid sign artifact with the challenge",
     () => { assert.equal(signCommand("sha256:ab"), "liquid sign artifact sha256:ab"); });

test("liquid sign output is recognised, with or without the signer line", () => {
    assert.equal(looksSigned("signature=a1b2\nsigner=taap:1:ed25519:X\n"), true);
    assert.equal(looksSigned("A1B2"), true);
    assert.equal(looksSigned(""), false);
    assert.equal(looksSigned("signature=xyz"), false);
    assert.equal(looksSigned("abc"), false);
    assert.equal(signedOutput("  signature=ab\n"), "signature=ab");
});

test("no page says custodial: the managed key and your own key (lake#762)", () => {
    const root = join(import.meta.dirname, "..", "src");
    const files = (dir: string): string[] => readdirSync(dir).flatMap((name) => {
        const path = join(dir, name);
        return statSync(path).isDirectory() ? files(path) : /\.tsx?$/.test(name) ? [ path ] : [];
    });
    const offenders = files(root).filter((path) => /custodial/i.test(readFileSync(path, "utf8")));
    assert.deepEqual(offenders, []);
});
