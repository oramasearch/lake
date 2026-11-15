// SPDX-License-Identifier: MIT

/**
 * SSO-only sign-in (lake#1136): a white-labelled Lake whose only way in is the customer's identity
 * provider offers that provider alone, and starts SAML and OIDC the way Lake's server expects.
 * `npm test`.
 */
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";

import {ssoButtonLabel, ssoStart} from "../src/lib/ssoSignIn.ts";

const signIn = readFileSync(new URL("../src/pages/account/SignInPage.tsx", import.meta.url), "utf8");
const signUp = readFileSync(new URL("../src/pages/account/SignUpPage.tsx", import.meta.url), "utf8");
const page = readFileSync(new URL("../src/pages/account/SsoSignIn.tsx", import.meta.url), "utf8");
// The page's code, without its opening comment (which explains what it leaves out).
const code = page.replace(/^\/\*\*[\s\S]*?\*\//, "");

test("SAML starts at Lake's sign-on URL and keeps only a same-origin return", () => {
    const saml = {provider_id: "sso_123", protocol: "saml", name: "Acme"};
    assert.deepEqual(ssoStart(saml, null), {kind: "redirect", url: "/v1/saml/sso_123/start"});
    assert.deepEqual(ssoStart(saml, "/repositories?x=1"),
                     {kind: "redirect", url: "/v1/saml/sso_123/start?return_to=%2Frepositories%3Fx%3D1"});
    for (const unsafe of ["//evil.example", "/\\evil.example", "https://evil.example/"]) {
        assert.deepEqual(ssoStart(saml, unsafe), {kind: "redirect", url: "/v1/saml/sso_123/start"});
    }
});

test("OIDC starts through start_sso_oidc", () => {
    assert.deepEqual(ssoStart({provider_id: "sso_9", protocol: "oidc", name: ""}, "/"), {kind: "oidc"});
    assert.match(page, /"start_sso_oidc"/);
});

test("the button names the customer's provider, never a product", () => {
    assert.equal(ssoButtonLabel({provider_id: "a", protocol: "saml", name: " Acme SSO "}), "Continue with Acme SSO");
    assert.equal(ssoButtonLabel({provider_id: "a", protocol: "saml", name: ""}), "Continue with your organization");
    assert.doesNotMatch(code, /\bLake\b|Clerk|Orama/);
});

test("both sign-in pages offer the provider alone when the viewer names one", () => {
    for (const source of [signIn, signUp]) {
        assert.match(source, /if \(viewer\.sign_in_sso\) return <SsoSignIn provider=\{viewer\.sign_in_sso\}/);
    }
    assert.doesNotMatch(code, /sign_in_password|start_oauth|passkey/i);
});
