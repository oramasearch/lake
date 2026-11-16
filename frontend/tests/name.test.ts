/**
 * The product's name is Lake, with a capital L, and the app is the Lake Console
 * (lake#642). Technical names keep their form: hostnames (`lake.example.com`, `lake.example.net`), the MCP
 * server name and other code values quoted alone (`"lake"`), identifiers (`lake_tok_`, `lake-theme`,
 * `LAKE_KEY`), paths (`/skills/lake/SKILL.md`), the ciphertext prefix `lake:v1:`, issue references
 * (`lake#642`) and anything inside `<code>`. Every other lowercase "lake" outside a comment is a
 * string or JSX text a person reads, so the check fails on it. `npm test`; the build runs it.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const ROOT = join(import.meta.dirname, "..", "src");

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? sources(path) : /\.tsx?$/.test(name) ? [path] : [];
  });
}

/** The source without its comments: block comments, JSX comments and line comments (a `//` after
 * `:` or a quote is a URL or a path, not a comment). */
function withoutComments(source: string): string {
  return source
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .split("\n")
    .map((line) => line.replace(/(^|[^:"'`])\/\/.*$/, "$1"))
    .join("\n");
}

/** Technical names that keep the lowercase form (see the header). */
const TECHNICAL = [
  /<code>.*?<\/code>/g, // a code span in JSX
  /(["'`])lake\1/g, // a code value quoted alone: the MCP server name, an option value
  /^\s*lake(?=\s*:)/g, // an object key
  /\blake(?=\.[A-Za-z])/g, // a hostname or dotted name: lake.live, lake.example
  /\blake:v\d/g, // the ciphertext prefix
  /\blake#\d+/g, // an issue reference
  /[\w\-./@$\\]lake\b/g, // inside a name or a path: oramasearch/lake, /skills/lake, .lake-folder
  /\blake[\w\-/\\]/g, // the start of a name or a path: lake_tok_, lake-theme, lake/…
];

const NAME = /\blake\b/;

/** Each line of a source file that names the product in lowercase to a reader. */
export function lowercaseNames(source: string): string[] {
  const found: string[] = [];
  for (const line of withoutComments(source).split("\n")) {
    let rest = line;
    for (const pattern of TECHNICAL) rest = rest.replace(pattern, " ");
    if (NAME.test(rest)) found.push(line.trim());
  }
  return found;
}

test("a lowercase lake is found in strings, templates and JSX text, not in technical names", () => {
  const lines = (text: string) => lowercaseNames(text).length;
  assert.equal(lines('const a = "Not available on this lake.";'), 1);
  assert.equal(lines("const b = `lake — ${title}`;"), 1);
  assert.equal(lines("<p>Ask anything about your lake.</p>"), 1);
  assert.equal(lines("        lake verifies and runs only an approved pipeline."), 1);
  assert.equal(lines('label: "lake domain",'), 1);
  assert.equal(lines("const c = \"Run lake's checks\";"), 1);
  assert.equal(lines('const ok = "Lake verifies it"; const title = "Lake Console";'), 0);
  assert.equal(lines("// lake in a comment\n/* lake */\n{/* the lake#577 decision */}"), 0);
  assert.equal(lines('const SERVER_NAME = "lake"; <option value="lake">Lake Sign-In</option>'), 0);
  assert.equal(lines("  lake: [\"M4 3h8v2H6v6h6v2H4z\"],"), 0);
  assert.equal(lines('fetch("https://lake.example.net/v1"); const home = "lake.example.com";'), 0);
  assert.equal(lines("const skill = `${origin}/skills/lake/SKILL.md`; const k = `lake-SKILL.md`;"), 0);
  assert.equal(lines('const key = "lake_tok_K"; localStorage.getItem("lake-theme"); env.LAKE_KEY;'), 0);
  assert.equal(lines('help="lake:v1:<key id>:<version>:<sealed>"'), 0);
  assert.equal(lines("<p>Run <code>claude mcp add lake</code> in a terminal.</p>"), 0);
  assert.equal(lines('const repo = "oramasearch/lake"; const id = "lake-key";'), 0);
});

test("the web app calls the product Lake, never lake", () => {
  const found: string[] = [];
  for (const file of sources(ROOT)) {
    for (const line of lowercaseNames(readFileSync(file, "utf8"))) found.push(`${relative(ROOT, file)}: ${line.slice(0, 160)}`);
  }
  assert.deepEqual(found, [], `say "Lake" (or "Lake Console" for the app itself); technical names keep their form:\n${found.join("\n")}`);
});

test("the page title before the app loads names the Lake Console", () => {
  const html = readFileSync(join(import.meta.dirname, "..", "index.html"), "utf8");
  assert.match(html, /<title>Lake Console<\/title>/);
});
