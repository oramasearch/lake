/** The keywords the web app avoids when it fills in a dataset name are the ones lake refuses
 * (lake#814): `SQL_KEYWORDS` is a copy of `keywords` in `src/api/datasets.cpp`, and this fails when
 * the two differ. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { SQL_KEYWORDS, isSqlKeyword, notAKeyword } from "../src/lib/sqlKeywords.ts";

/** The words of the server's `keywords` array, read from its source. */
function serverKeywords(): string[] {
  const source = readFileSync(new URL("../../src/api/datasets.cpp", import.meta.url), "utf8");
  const array = /constexpr\s+std::array<std::string_view,\s*(\d+)>\s+keywords\s*=\s*\{([^}]*)\}/.exec(source);
  assert.ok(array, "src/api/datasets.cpp declares `keywords` as a constexpr std::array");
  const words = [...array[2].matchAll(/"([^"]*)"/g)].map((match) => match[1]);
  assert.equal(words.length, Number(array[1]), "the array's declared size matches its words");
  return words;
}

test("the web app's SQL keywords are the server's, word for word", () => {
  assert.deepEqual([...SQL_KEYWORDS].sort(), serverKeywords().sort());
});

test("a keyword is matched as the server matches it, ignoring case", () => {
  assert.equal(isSqlKeyword("order"), true);
  assert.equal(isSqlKeyword("GROUP"), true);
  assert.equal(isSqlKeyword("orders"), false);
});

test("a keyword gets _data and any other name is kept", () => {
  assert.equal(notAKeyword("order"), "order_data");
  assert.equal(notAKeyword("Order"), "Order_data");
  assert.equal(notAKeyword("customers"), "customers");
});
