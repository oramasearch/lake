/**
 * What a person reads in the web app: an AI training experiment, as an experiment tracker says it,
 * never a "study" (Steve, 2026-10-10, lake#561). The engine calls an experiment a study, so its
 * names may appear in code: `list_studies` and `study_id` (an underscore joins them to the word), the
 * `study-events.<id>` topic, the `study.id` run parameter, and ExperimentsAgent's `study` field read
 * as `result.study` or written as `study:`, or quoted alone as `"study"`. Every other "study" or
 * "studies" outside a comment is a string or JSX text a person can read, so the check fails on it.
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

/** The source without comments (`//` to the end of the line, unless it follows a `:` as in a URL,
 * and `/* … *\/`). */
function withoutComments(source: string): string {
  return source.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/(^|[^:])\/\/.*$/gm, "$1");
}

/** The engine's names, as code reads and writes them (see the header). */
const ENGINE = [/\bstudy-events\./g, /\bstudy\.id\b/g, /\?*\.stud(y|ies)\b/g, /\bstudy(?=\s*:\s*[A-Za-z_(])/g, /(["'`])study\1/g];

const STUDY = /\bstud(y|ies)\b/gi;

/** Each line of a source file that says study or studies to a reader. */
export function studyLines(source: string): string[] {
  const found: string[] = [];
  for (const line of withoutComments(source).split("\n")) {
    let rest = line;
    for (const pattern of ENGINE) rest = rest.replace(pattern, "");
    if (STUDY.test(rest)) found.push(line.trim());
    STUDY.lastIndex = 0;
  }
  return found;
}

test("a reader's study is found in strings, templates and JSX text, not in comments or engine names", () => {
  const lines = (text: string) => studyLines(text).length;
  assert.equal(lines('const a = "Start study";'), 1);
  assert.equal(lines("const b = `Trial ${n} of study ${name}`;"), 1);
  assert.equal(lines("<Card title='Studies'>No studies yet.</Card>"), 1);
  assert.equal(lines("      {n ? `Trial ${n} of ` : \"A trial of \"}study{\" \"}"), 1);
  assert.equal(lines("label: \"Study\","), 1);
  assert.equal(lines("// a study in a comment\n/* the studies */"), 0);
  assert.equal(lines('const q = useWq<Row>("list_studies"); const id = text(run, "study_id");'), 0);
  assert.equal(lines("const topic = `study-events.${id}`; params[\"study.id\"];"), 0);
  assert.equal(lines("const ok = result.study && listing?.studies; send({ ...proposal, study: experiment });"), 0);
  assert.equal(lines('const old = params.get("study");'), 0);
  assert.equal(lines("fetch(\"https://example.test\"); // a study"), 0);
});

test("the web app says experiment, never study", () => {
  const found: string[] = [];
  for (const file of sources(ROOT)) {
    for (const line of studyLines(readFileSync(file, "utf8"))) found.push(`${relative(ROOT, file)}: ${line.slice(0, 160)}`);
  }
  assert.deepEqual(found, [], `say "experiment" (and "trial" for one run of an experiment's settings):\n${found.join("\n")}`);
});
