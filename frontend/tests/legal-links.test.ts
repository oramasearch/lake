/**
 * Terms of service and privacy policy on sign-up and sign-in (liquid-bridge#15): every form, Clerk's
 * and lake's own, carries the sentence with the links `viewer.legal` reports, and none when the
 * instance links to neither. `npm test`.
 */
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";

import {legalNotice, PRIVACY_LABEL, safeLegalUrl, TERMS_LABEL} from "../src/lib/legalLinks.ts";

const read = (path: string) => readFileSync(new URL(path, import.meta.url), "utf8");
const TERMS = "https://www.orama.com/legal/terms";
const PRIVACY = "https://www.orama.com/legal/privacy";
const textOf = (parts: ReturnType<typeof legalNotice>) =>
    (parts ?? []).map((part) => ("href" in part ? `[${part.label}](${part.href})` : part.text)).join("");

test("sign-up agrees to the terms and acknowledges the privacy policy, both linked", () => {
    assert.equal(
        textOf(legalNotice("sign-up", {terms_url: TERMS, privacy_url: PRIVACY})),
        `By creating an account, you agree to the [${TERMS_LABEL}](${TERMS}) and acknowledge the [${PRIVACY_LABEL}](${PRIVACY}).`);
    assert.equal(
        textOf(legalNotice("sign-in", {terms_url: TERMS, privacy_url: PRIVACY})),
        `By signing in, you agree to the [${TERMS_LABEL}](${TERMS}) and acknowledge the [${PRIVACY_LABEL}](${PRIVACY}).`);
});

test("one document alone is linked alone, and neither shows nothing", () => {
    assert.equal(
        textOf(legalNotice("sign-up", {terms_url: TERMS, privacy_url: ""})),
        `By creating an account, you agree to the [${TERMS_LABEL}](${TERMS}).`);
    assert.equal(
        textOf(legalNotice("sign-in", {terms_url: "", privacy_url: PRIVACY})),
        `How we handle your data: [${PRIVACY_LABEL}](${PRIVACY}).`);
    assert.equal(legalNotice("sign-up", {terms_url: "", privacy_url: ""}), null);
    assert.equal(legalNotice("sign-up", undefined), null);
});

test("only absolute http and https URLs become links", () => {
    assert.equal(safeLegalUrl(TERMS), TERMS);
    assert.equal(safeLegalUrl("http://localhost:3000/terms"), "http://localhost:3000/terms");
    for (const bad of ["javascript:alert(1)", "/legal/terms", "data:text/html,x", "", undefined]) {
        assert.equal(safeLegalUrl(bad), "", String(bad));
    }
    assert.equal(legalNotice("sign-up", {terms_url: "javascript:alert(1)", privacy_url: ""}), null);
});

test("every sign-up and sign-in form renders the notice, and its links open in a new tab", () => {
    const clerk = read("../src/pages/account/ClerkSignIn.tsx");
    assert.match(clerk, /<LegalNotice mode=\{signUp \? "sign-up" : "sign-in"\} \/>/);
    assert.match(read("../src/pages/account/SignUpPage.tsx"), /<LegalNotice mode="sign-up" \/>/);
    assert.match(read("../src/pages/account/SignInPage.tsx"), /<LegalNotice mode="sign-in" \/>/);
    const notice = read("../src/pages/account/LegalNotice.tsx");
    assert.match(notice, /legalNotice\(mode, viewer\.legal\)/);
    assert.match(notice, /target="_blank" rel="noopener noreferrer"/);
});
