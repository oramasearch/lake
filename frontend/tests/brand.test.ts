/**
 * A white-labelled Lake (lake#1135): the console names itself from `viewer.brand`, never from a
 * literal, and Lake's own brand reads exactly as before. `npm test`.
 */
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";

import {
    brandKicker,
    brandOf,
    brandText,
    documentTitle,
    instanceBrand,
    LAKE_BRAND
} from "../src/lib/brand.ts";

const acme = brandOf({
    brand : {
        custom : true,
        name : "Acme Data",
        short_name : "acme-data",
        company : "Acme",
        assistant_name : "Ask Acme",
        console_title : "Acme Data Console",
        eyebrow : "Acme Data",
        by_orama : false,
        logo : "data:image/png;base64,iVBORw0KGgo=",
        accent : "#aa3300",
        support_email : "help@acme.example",
        legal_name : "Acme Data Inc.",
    },
});

const source = (path: string) => readFileSync(new URL(path, import.meta.url), "utf8");

test("Lake's own brand reads exactly as before", () => {
    assert.deepEqual(brandOf(null), {...LAKE_BRAND});
    assert.deepEqual(brandOf({}), {...LAKE_BRAND});
    // Outside a browser there is no embedded viewer: Lake's brand.
    assert.equal(instanceBrand().name, "Lake");
    assert.equal(documentTitle(LAKE_BRAND, "Projects"), "Lake — Projects");
    assert.equal(documentTitle(LAKE_BRAND, undefined), "Lake Console");
    assert.equal(brandKicker(LAKE_BRAND), "Orama");
    const copy = "Ask about anything in your Lake. Ask Orama proposes; the Lake Console shows it.";
    assert.equal(brandText(LAKE_BRAND, copy), copy);
});

test("a custom brand rewrites Lake's copy with no Lake or Orama left", () => {
    const copy = [
        "Ask about anything in your Lake. Changes are proposed for you to confirm.",
        "Sign-in could not load: Clerk is not configured on this Lake.",
        "Create your Lake account with your email address or one of the accounts below.",
        "Lake by Orama",
        "Lake Console",
        "Ask Orama",
        "the Orama operator",
        "Orama's AI solutions",
        "lake.live is prepaid",
    ].join("\n");
    const branded = brandText(acme, copy);
    assert.doesNotMatch(branded, /\bLake\b|Orama|lake\.live/);
    assert.match(branded, /your Acme Data\. Changes/);
    assert.match(branded, /^Acme Data$/m);
    assert.match(branded, /Acme Data Console/);
    assert.match(branded, /Ask Acme/);
    assert.match(branded, /the operator/);
    assert.match(branded, /Acme's AI solutions/);
    // Identifiers and words that only contain the name are left alone.
    assert.equal(brandText(acme, "LakeSet lake_session Lakes"), "LakeSet lake_session Lakes");
    assert.equal(documentTitle(acme, "Projects"), "Acme Data — Projects");
    assert.equal(documentTitle(acme, null), "Acme Data Console");
    assert.equal(brandKicker(acme), "");
});

test("the shell, the sign-in pages and Ask name the product from the brand", () => {
    const shell = source("../src/components/Shell.tsx");
    const frame = source("../src/pages/account/AuthFrame.tsx");
    const clerk = source("../src/pages/account/ClerkSignIn.tsx");
    const ask = source("../src/components/AskPanel.tsx");
    const nav = source("../src/routes/nav.ts");
    const code = (text: string) =>
        text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
    for (const [name, text] of [[ "Shell", shell ],
                                [ "AuthFrame", frame ],
                                [ "ClerkSignIn", clerk ],
                                [ "AskPanel", ask ],
                                [ "nav", nav ]]) {
        // brandText's argument is Lake's copy, rewritten at render; everything else the person
        // sees names no product.
        const body = code(text).replace(/brandText\([^,()]+(?:\(\))?,\s*"[^"]*"\)/g, "brandText()");
        const literals =
            [...body.matchAll(/"([^"\n]*)"|>([^<>{}\n]+)</g) ].map((m) => m[1] ?? m[2]);
        const left = literals.filter((t) => /\bLake\b|Orama/.test(t));
        assert.deepEqual(left, [], `${name} names Lake or Orama itself: ${left.join(" | ")}`);
        assert.doesNotMatch(body, /`Lake —|"Lake Console"|"Lake by Orama"|>Lake<|>Orama</, name);
    }
    assert.match(shell, /documentTitle\(instanceBrand\(\), title\)/);
    assert.match(shell, /<BrandMark brand=\{brand\} \/>/);
    assert.match(frame, /<BrandMark brand=\{brand\} size=\{30\} \/>/);
    assert.match(clerk, /eyebrow=\{instanceBrand\(\)\.eyebrow\}/);
    assert.match(clerk, /colorPrimary: instanceBrand\(\)\.accent \?\? "#0f62fe"/);
    assert.match(nav, /label: instanceBrand\(\)\.assistant_name/);
});
