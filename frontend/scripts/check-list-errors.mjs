// A failed read never looks like an empty list. Fails the build when an empty state can
// render from `q.data ?? []` without the query's error being checked first:
//   1. a <DataTable … empty={…}> whose rows trace back to `<query>.data … ?? []` must carry
//      `q={<query>}` (the table then renders loading and the error before any empty state), or
//      sit in a file that checks that query's error;
//   2. an <Empty> guarded by `<x>.length` where <x> traces back to `<query>.data … ?? []` must sit
//      in a file that checks that query's error (`<query>.isError`, `q={<query>}`).
// Rows that arrive through <Query q={…}>{(rows) => …}</Query> are already guarded and pass.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

function files(dir) {
  return readdirSync(dir).flatMap((f) => {
    const p = join(dir, f);
    return statSync(p).isDirectory() ? files(p) : p.endsWith(".tsx") ? [p] : [];
  });
}

/** The opening tag of a JSX element starting at `start` (`<Name`), braces balanced. */
function openingTag(text, start) {
  let depth = 0;
  for (let i = start + 1; i < text.length; i++) {
    const c = text[i];
    if (c === "{") depth++;
    else if (c === "}") depth--;
    else if (c === ">" && depth === 0 && text[i - 1] !== "=") return text.slice(start, i + 1);
  }
  return text.slice(start);
}

/** The `{…}` expression of a prop inside an opening tag, braces balanced. */
function propExpr(tag, name) {
  const m = new RegExp(String.raw`\s${name}=\{`).exec(tag);
  if (!m) return null;
  let depth = 1;
  const from = m.index + m[0].length;
  for (let i = from; i < tag.length; i++) {
    if (tag[i] === "{") depth++;
    else if (tag[i] === "}" && --depth === 0) return tag.slice(from, i);
  }
  return null;
}

const FALLBACK = /(\w+)\.data(?:\?\.[\w.?]+)?\s*\?\?\s*\[\]/;

/** The query a list expression comes from via `<query>.data … ?? []`, following const initializers. */
function sourceQuery(text, expr, seen = new Set()) {
  const direct = FALLBACK.exec(expr);
  if (direct) return direct[1];
  for (const id of expr.match(/\b[a-zA-Z_]\w*\b/g) ?? []) {
    if (seen.has(id)) continue;
    seen.add(id);
    const decl = new RegExp(String.raw`const\s+${id}\s*(?::[^=]+)?=\s*([^;]+);`).exec(text);
    if (!decl) continue;
    const q = sourceQuery(text, decl[1], seen);
    if (q) return q;
  }
  return null;
}

const errorChecked = (text, q) => new RegExp(String.raw`\b${q}\.isError\b|\bq=\{${q}\}`).test(text);

const found = [];
for (const file of files("src")) {
  const text = readFileSync(file, "utf8");
  const line = (i) => text.slice(0, i).split("\n").length;

  for (const m of text.matchAll(/<DataTable\b/g)) {
    const tag = openingTag(text, m.index);
    if (!/\sempty=\{/.test(tag) || /\sq=\{/.test(tag)) continue;
    const rows = propExpr(tag, "rows");
    const q = rows && sourceQuery(text, rows);
    if (q && !errorChecked(text, q)) found.push(`${file}:${line(m.index)} DataTable rows come from ${q}.data ?? [] — pass q={${q}}`);
  }

  for (const m of text.matchAll(/<Empty\b/g)) {
    const before = text.slice(Math.max(0, m.index - 240), m.index);
    for (const g of before.matchAll(/([\w.()?\s[\]]+?)\.length\b/g)) {
      const q = sourceQuery(text, g[1]);
      if (q && !errorChecked(text, q)) {
        found.push(`${file}:${line(m.index)} empty state guarded by ${g[1].trim()}.length from ${q}.data ?? [] — check ${q}.isError first`);
        break;
      }
    }
  }
}

if (found.length) {
  console.error(`An empty state can render for a failed read:\n  ${found.join("\n  ")}`);
  process.exit(1);
}
console.log("every empty state checks for a failed read");
