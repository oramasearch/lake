/** Words the web app shows (lake#583): no page names Zetta to a person. The rest of the app speaks
 * of lake and the workspace (docs/ui/web-app.md, "Words"). Comments and identifiers (`hasZetta`,
 * `bindings.zetta`) may name it; text a person reads may not. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? sources(path) : /\.tsx?$/.test(name) ? [path] : [];
  });
}

/** The source without its comments: block comments, JSX comments and line comments. */
export function withoutComments(source: string): string {
  return source
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .split("\n")
    .map((line) => line.replace(/(^|[^:"'`])\/\/.*$/, "$1"))
    .join("\n");
}

/** Every line that names Zetta as a word, not inside an identifier. */
export function engineNames(source: string): string[] {
  return withoutComments(source)
    .split("\n")
    .filter((line) => /(?<![A-Za-z0-9_])Zetta\b/.test(line));
}

test("the check finds Zetta in text and not in comments or identifiers", () => {
  assert.deepEqual(engineNames(`const a = "Zetta builds the steps";`), [`const a = "Zetta builds the steps";`]);
  assert.deepEqual(engineNames(`// Zetta's own shape\nconst hasZetta = true;`), []);
  assert.deepEqual(engineNames(`/** The workspace Zetta. */\n<p>{hasZetta ? "Attached" : "Detached"}</p>`), []);
});

test("no page text names Zetta", () => {
  const found: string[] = [];
  for (const path of sources(join(import.meta.dirname, "..", "src"))) {
    for (const line of engineNames(readFileSync(path, "utf8"))) found.push(`${path}: ${line.trim()}`);
  }
  assert.deepEqual(found, [], "say lake or the workspace instead");
});
