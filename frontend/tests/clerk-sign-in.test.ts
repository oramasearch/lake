/**
 * The Clerk sign-in page (lake#1115, lake#1116): it stops posting after Lake refuses a token, and
 * it speaks of invitations only where the Clerk instance's sign-up is by invitation. `npm test`.
 */
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";

import {
    clerkPageSub,
    exchangeAutomatically,
    INVITATION_NOTE,
    signUpByInvitation,
    signUpModeOf,
} from "../src/lib/clerkSignIn.ts";

const page = readFileSync(new URL("../src/pages/account/ClerkSignIn.tsx", import.meta.url), "utf8");
const home = readFileSync(new URL("../src/pages/home/HomePage.tsx", import.meta.url), "utf8");

test("after a refused exchange the page waits for the person instead of posting again", () => {
    assert.equal(exchangeAutomatically(""), true);
    assert.equal(exchangeAutomatically("?next=%2Fdocs"), true);
    assert.equal(
        exchangeAutomatically(
            "?error=Your%20sign-in%20could%20not%20be%20verified.%20Please%20try%20again."),
        false);
    assert.equal(exchangeAutomatically("?signed_out=1&error=x"), false);
    // The page reads it once, holds instead of exchanging, and offers the two ways on.
    assert.match(
        page,
        /const \[automatic\] = useState\(\(\) => exchangeAutomatically\(location\.search\)\)/);
    assert.match(page, /if \(!automatic\) \{[^}]*setHeld\(true\);\s*return;/s);
    assert.match(page, />\s*Try again\s*</);
    assert.match(page, />\s*Use another account\s*</);
});

test("the sign-up mode comes from what clerk-js fetched, never a second setting", () => {
    const env = (mode: unknown) => ({__unstable__environment : {userSettings : {signUp : {mode}}}});
    assert.equal(signUpModeOf(env("public")), "public");
    assert.equal(signUpModeOf(env("restricted")), "restricted");
    assert.equal(signUpModeOf(env("waitlist")), "waitlist");
    assert.equal(signUpModeOf(env("something-new")), "unknown");
    assert.equal(signUpModeOf({}), "unknown");
    assert.equal(signUpModeOf(null), "unknown");
    assert.equal(signUpByInvitation("restricted"), true);
    assert.equal(signUpByInvitation("waitlist"), true);
    assert.equal(signUpByInvitation("public"), false);
    assert.equal(signUpByInvitation("unknown"), false);
});

test("invitation wording appears only where sign-up is by invitation", () => {
    assert.match(clerkPageSub(true, "restricted"),
                 new RegExp(`^${INVITATION_NOTE.replace(/\./g, "\\.")}`));
    assert.equal(clerkPageSub(false, "restricted"),
                 "Sign in with the email address you were invited with.");
    for (const mode of ["public", "unknown"] as const) {
        for (const signUp of [true, false]) {
            const sub = clerkPageSub(signUp, mode);
            assert.doesNotMatch(sub, /invit/i, `${mode} ${signUp}`);
        }
    }
    assert.match(page, /sub=\{brandText\(instanceBrand\(\), clerkPageSub\(signUp, signUpMode\)\)\}/);
    assert.doesNotMatch(page, /invited with/);
    // The visitor homepage: the note only under restricted sign-up, Create an account under public.
    assert.match(home, /signUpByInvitation\(signUpMode\) \?/);
    assert.match(home, /signUpMode === "public" \?[\s\S]*Create an account/);
});
