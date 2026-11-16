// SPDX-License-Identifier: MIT

/** The words the workspace's SQL subset reads as syntax, so a dataset named one cannot be queried
 * and lake refuses it ("'order' is a SQL keyword; choose another name"). A copy of `keywords` in
 * `src/api/datasets.cpp`; `tests/sql-keywords.test.ts` fails when the two lists differ (lake#814). */
export const SQL_KEYWORDS: readonly string[] = [
  "all", "and", "as", "asc", "by", "desc", "distinct", "from", "group",
  "having", "in", "is", "join", "like", "limit", "not", "null", "offset",
  "on", "or", "order", "select", "union", "where", "with",
];

const KEYWORDS = new Set(SQL_KEYWORDS);

/** Whether lake refuses `name` as a SQL keyword; the server compares lower-cased. */
export function isSqlKeyword(name: string): boolean {
  return KEYWORDS.has(name.toLowerCase());
}

/** A name filled in for the person that lake would refuse as a keyword gets `_data`
 * (`order` → `order_data`); any other name is kept. */
export function notAKeyword(name: string): string {
  return isSqlKeyword(name) ? `${name}_data` : name;
}
