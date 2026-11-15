// SPDX-License-Identifier: MIT

/**
 * The catalogue icons (lake#639, src/lib/catalogueIcons.ts): every solution and methodology in
 * Orama's catalogue has its own glyph, every glyph keeps to the grid and the two tones, a new entry
 * falls back to its original's, its methodology's or its family's glyph and then a monogram, and the
 * website's copy under assets/catalogue-icons/ is the one the source writes.
 */
import assert from "node:assert/strict";
import { test } from "node:test";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { join, relative } from "node:path";

import {
  CATALOGUE_NAMES,
  ENGINE_FAMILIES,
  FAMILY_GLYPHS,
  METHODOLOGY_GLYPHS,
  SOLUTION_GLYPHS,
  familyKey,
  methodologyIcon,
  monogramOf,
  primaryMethodology,
  solutionIcon,
  standaloneFiles,
  type Glyph,
  type Mark,
} from "../src/lib/catalogueIcons.ts";

/** The catalogue as the engine lists it (zetta dev: `solution_table.cpp`, `*_methodology.cpp`). */
const SOLUTIONS = ["S-1", "S-2", "S-3", "S-4", "S-5", "S-6", "S-7", "S-8", "S-9", "S-10", "S-11", "S-12", "B-1", "B-2", "B-3", "B-4", "B-5", "B-6", "B-7", "B-8"];
const METHODOLOGIES = Array.from({ length: 21 }, (_, i) => `M-${i + 1}`);

const ALL: [string, Glyph][] = [...Object.entries(METHODOLOGY_GLYPHS), ...Object.entries(SOLUTION_GLYPHS), ...Object.entries(FAMILY_GLYPHS).map(([k, g]) => [`family:${k}`, g] as [string, Glyph])];

test("every solution and methodology in the catalogue has its own glyph and name", () => {
  for (const id of SOLUTIONS) {
    assert.ok(SOLUTION_GLYPHS[id], `${id} has a glyph`);
    assert.ok(CATALOGUE_NAMES[id], `${id} has a name`);
    assert.equal(solutionIcon({ id }).from, "catalogue", `${id} draws its own glyph`);
  }
  for (const id of METHODOLOGIES) {
    assert.ok(METHODOLOGY_GLYPHS[id], `${id} has a glyph`);
    assert.ok(CATALOGUE_NAMES[id], `${id} has a name`);
    assert.equal(methodologyIcon({ id }).from, "catalogue", `${id} draws its own glyph`);
  }
  assert.deepEqual(Object.keys(SOLUTION_GLYPHS).sort(), [...SOLUTIONS].sort(), "no glyph for an entry the catalogue does not list");
  assert.deepEqual(Object.keys(METHODOLOGY_GLYPHS).sort(), [...METHODOLOGIES].sort());
});

test("no two glyphs are the same picture, and each says what it shows", () => {
  const pictures = new Map<string, string>();
  const labels = new Set<string>();
  for (const [id, glyph] of ALL) {
    const picture = JSON.stringify(glyph.marks);
    assert.ok(!pictures.has(picture), `${id} draws the same picture as ${pictures.get(picture)}`);
    pictures.set(picture, id);
    assert.ok(glyph.label.length > 8, `${id} has an accessible label`);
    assert.ok(!labels.has(glyph.label), `${id}'s label is its own`);
    labels.add(glyph.label);
    assert.ok(glyph.motifs.length > 0, `${id} names its motifs`);
  }
});

/** Every point a path passes through (end points of each segment), in absolute coordinates. */
function pathPoints(d: string): [number, number][] {
  const tokens = d.match(/[a-zA-Z]|-?\d*\.?\d+(?:e-?\d+)?/g) ?? [];
  const points: [number, number][] = [];
  let x = 0;
  let y = 0;
  let startX = 0;
  let startY = 0;
  let command = "";
  let i = 0;
  const num = () => Number(tokens[i++]);
  const ARITY: Record<string, number> = { m: 2, l: 2, h: 1, v: 1, c: 6, s: 4, q: 4, t: 2, a: 7, z: 0 };
  while (i < tokens.length) {
    if (/[a-zA-Z]/.test(tokens[i])) command = tokens[i++];
    const lower = command.toLowerCase();
    const rel = command === lower;
    if (lower === "z") {
      x = startX;
      y = startY;
      continue;
    }
    const args = Array.from({ length: ARITY[lower] }, num);
    assert.ok(args.every((n) => Number.isFinite(n)), `path ${d} parses`);
    if (lower === "h") x = rel ? x + args[0] : args[0];
    else if (lower === "v") y = rel ? y + args[0] : args[0];
    else {
      const [ex, ey] = args.slice(-2);
      x = rel ? x + ex : ex;
      y = rel ? y + ey : ey;
    }
    if (lower === "m") {
      startX = x;
      startY = y;
      command = rel ? "l" : "L";
    }
    points.push([x, y]);
  }
  return points;
}

function bounds(mark: Mark): [number, number][] {
  if (mark.kind === "circle") return [[mark.cx - mark.r, mark.cy - mark.r], [mark.cx + mark.r, mark.cy + mark.r]];
  if (mark.kind === "rect") return [[mark.x, mark.y], [mark.x + mark.w, mark.y + mark.h]];
  return pathPoints(mark.d);
}

test("every glyph keeps to the 24-unit grid's live area and uses the two tones", () => {
  for (const [id, glyph] of ALL) {
    for (const mark of glyph.marks) {
      for (const [x, y] of bounds(mark)) {
        assert.ok(x >= 2 && x <= 22.5 && y >= 2 && y <= 22.5, `${id}: (${x}, ${y}) is inside 2–22`);
      }
    }
    assert.ok(glyph.marks.some((m) => m.tone === "line"), `${id} has an ink line`);
    assert.ok(glyph.marks.some((m) => m.tone !== "line"), `${id} has an accent`);
    // A tint sits behind the lines: it is drawn before any line it fills.
    const firstLine = glyph.marks.findIndex((m) => m.tone === "line");
    for (const [index, mark] of glyph.marks.entries()) if (mark.tone === "tint") assert.ok(index < firstLine, `${id}: its tint is drawn first`);
  }
});

test("a methodology the set has not drawn takes its family's glyph, then a monogram", () => {
  for (const [family, key] of Object.entries(ENGINE_FAMILIES)) assert.equal(familyKey(family), key);
  const next = methodologyIcon({ id: "M-40", name: "Causal discovery", family: "Action & decision" });
  assert.equal(next.from, "family");
  assert.equal(next.key, "family:action");
  const unknown = methodologyIcon({ id: "M-41", name: "quantum annealing", family: "Something new" });
  assert.deepEqual([unknown.from, unknown.key], ["monogram", "monogram:Q"]);
});

test("a solution of your own takes its original's glyph, then its methodology's, then a monogram", () => {
  assert.equal(solutionIcon({ id: "a1", name: "Our churn", duplicated_from: "S-1" }).key, "S-1");
  const built = solutionIcon({ id: "a2", name: "Late payments", methodologies: [{ id: "M-3" }, { id: "M-1", role: "primary" }] });
  assert.deepEqual([built.from, built.key], ["methodology", "M-1"], "the primary methodology, wherever it is listed");
  assert.equal(primaryMethodology({ methodologies: [{ id: "M-3" }, { id: "M-1" }] }), "M-3", "else the first");
  assert.equal(primaryMethodology({ composition: [{ methodology: "M-7" }] }), "M-7");
  const later = solutionIcon({ id: "a3", name: "Routing", methodologies: ["M-40"] }, { "M-40": "Action & decision" });
  assert.deepEqual([later.from, later.key], ["family", "family:action"]);
  const bare = solutionIcon({ id: "a4", name: "  élan vital", methodologies: [] });
  assert.deepEqual([bare.from, bare.key, bare.label], ["monogram", "monogram:É", "The letter É"]);
  assert.equal(monogramOf(""), "?");
  assert.equal(solutionIcon(null).key, "monogram:?");
});

test("the website's copy under assets/catalogue-icons is the one the source writes", () => {
  const root = join(import.meta.dirname, "..", "..", "assets", "catalogue-icons");
  const files = standaloneFiles();
  for (const [path, body] of Object.entries(files)) {
    assert.ok(existsSync(join(root, path)), `${path} is exported (node tools/ui/export-catalogue-icons.mjs)`);
    assert.equal(readFileSync(join(root, path), "utf8"), body, `${path} is current (node tools/ui/export-catalogue-icons.mjs)`);
    assert.ok(!/<image|gradient|filter|data:/i.test(body), `${path} is plain vector`);
  }
  const walk = (dir: string): string[] => readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(join(dir, e.name)) : [relative(root, join(dir, e.name))]));
  assert.deepEqual(walk(root).filter((p) => p.endsWith(".svg")).sort(), Object.keys(files).sort(), "no stale file");
});
