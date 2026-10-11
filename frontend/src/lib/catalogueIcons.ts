/** The catalogue icons (lake#639): one hand-drawn glyph for every solution and methodology in
 * Orama's catalogue, the family glyphs a methodology the set does not know yet falls back to, and
 * the rule that picks a glyph for any entry, a workspace's own solutions included.
 *
 * This file is the source of record. Each glyph is drawn on a 24 × 24 grid with a 2-unit margin
 * (live area 2–22), as marks in four tones:
 *
 * - `line`: a 1.5-unit stroke in `currentColor` (the ink), round caps and joins;
 * - `accent`: the same stroke in the accent (`--cat-accent`: blue for a methodology, amber for a
 *   solution), the part of the picture that is the intelligence or the decision;
 * - `dot`: a solid fill in the accent, for points and markers, no stroke;
 * - `tint`: a flat fill in the accent at low strength (`--cat-tint`), behind the lines.
 *
 * `components/CatalogueIcon.tsx` draws them; `docs/ui/catalogue-icons.md` is the style sheet and the
 * motif dictionary; `tools/ui/export-catalogue-icons.mjs` writes each as a standalone SVG for the
 * website. Pure data and logic, so `tests/catalogue-icons.test.ts` runs it under Node. */

export type Tone = "line" | "accent" | "dot" | "tint";

/** One mark: a path, a circle or a rounded rectangle, in a tone, optionally dashed. */
export type Mark =
  | { kind: "path"; d: string; tone: Tone; dash?: boolean }
  | { kind: "circle"; cx: number; cy: number; r: number; tone: Tone; dash?: boolean }
  | { kind: "rect"; x: number; y: number; w: number; h: number; rx: number; tone: Tone; dash?: boolean };

export type Glyph = {
  /** What the picture shows, read by a screen reader. */
  label: string;
  /** The motifs it is built from (the dictionary in docs/ui/catalogue-icons.md). */
  motifs: string[];
  marks: Mark[];
};

const p = (d: string, tone: Tone = "line", dash = false): Mark => ({ kind: "path", d, tone, ...(dash ? { dash } : {}) });
const c = (cx: number, cy: number, r: number, tone: Tone = "line", dash = false): Mark => ({ kind: "circle", cx, cy, r, tone, ...(dash ? { dash } : {}) });
const r = (x: number, y: number, w: number, h: number, rx: number, tone: Tone = "line", dash = false): Mark => ({ kind: "rect", x, y, w, h, rx, tone, ...(dash ? { dash } : {}) });

// Shared motifs, so a solution and the methodologies it composes look related.
const person = (x: number, y: number): Mark[] => [c(x, y, 2.75), p(`M${x - 4.5} 20c0-3.3 2-5.25 4.5-5.25s4.5 1.95 4.5 5.25`)];
const shield = (marks: Mark[] = []): Mark[] => [p("M12 3l7 2.75V11c0 4.4-2.9 8-7 10-4.1-2-7-5.6-7-10V5.75z"), ...marks];
const page = (x: number, right: number, bottom: number): Mark[] => [p(`M${x} 3h${right - x - 4}l4 4v${bottom - 7}H${x}z`), p(`M${right - 4} 3v4h4`)];

/** Orama's methodologies (M-…), as the engine registers them (zetta `src/core/ml/*_methodology.cpp`). */
export const METHODOLOGY_GLYPHS: Record<string, Glyph> = {
  "M-1": {
    label: "A table whose last column, the one predicted, is highlighted",
    motifs: ["table", "predicted column"],
    marks: [r(15, 4, 6, 16, 0, "tint"), r(3, 4, 18, 16, 2), p("M3 9.5h18M3 15h18M9 4v16"), p("M15 4v16", "accent")],
  },
  "M-2": {
    label: "Two records joined into one",
    motifs: ["record", "merge"],
    marks: [c(17.5, 12, 3.75, "tint"), c(5.5, 6.5, 2.5), c(5.5, 17.5, 2.5), p("M7.8 7.6c3 1 4.6 2.6 6 3.6M7.8 16.4c3-1 4.6-2.6 6-3.6", "accent"), c(17.5, 12, 3.75, "accent")],
  },
  "M-3": {
    label: "Points gathered into two groups, one highlighted",
    motifs: ["cluster"],
    marks: [c(16.5, 16, 5, "tint"), c(8, 8, 5), c(7, 6.75, 1, "dot"), c(9.5, 8.5, 1, "dot"), c(6.75, 9.75, 1, "dot"), c(16.5, 16, 5, "accent"), c(15.25, 14.75, 1, "dot"), c(18, 15.5, 1, "dot"), c(16.25, 18, 1, "dot")],
  },
  "M-4": {
    label: "A line of past values continued as a dashed forecast with its range",
    motifs: ["history line", "dashed future", "range cone"],
    marks: [p("M12.5 9.5L21 4v8z", "tint"), p("M3 20.5h18"), p("M3 16l3-3.5 3 2 3.5-5"), p("M12.5 9.5l3 1.5 2.5-3 3-1", "accent", true)],
  },
  "M-5": {
    label: "A survival curve stepping down over time",
    motifs: ["axes", "survival steps"],
    marks: [p("M3 3v18h18"), p("M6 5.5h3.5V9H13v4h3v4h4", "accent"), c(20, 17, 1.25, "dot")],
  },
  "M-6": {
    label: "A treated bar rising above the control's level",
    motifs: ["two groups", "lift arrow"],
    marks: [r(14, 7, 6, 13, 1.5, "tint"), r(4, 12, 6, 8, 1.5), r(14, 7, 6, 13, 1.5), p("M3 12h18", "accent", true), p("M17 16.5v-6M15 12.5l2-2 2 2", "accent")],
  },
  "M-7": {
    label: "A steady line with one spike marked",
    motifs: ["signal line", "spike"],
    marks: [p("M3 15.5l3-1 2.5 1.5 2-1M15 16l2.5-1.5 2 1 1.5-.5"), p("M10.5 15l2.5-9.5 2 10.5", "accent"), c(13, 4.75, 1.75, "dot")],
  },
  "M-8": {
    label: "An arrow striking the centre of a target",
    motifs: ["target", "arrow"],
    marks: [c(11, 13, 8), c(11, 13, 4.25), p("M11 13l8.5-8.5M16.5 4h3.5v3.5", "accent"), c(11, 13, 1.75, "dot")],
  },
  "M-9": {
    label: "Three items in ranked order, the first highlighted",
    motifs: ["ranked list"],
    marks: [p("M9 12h8M9 18h5"), c(5, 12, 1.25, "dot"), c(5, 18, 1.25, "dot"), p("M9 6h11", "accent"), c(5, 6, 1.75, "dot")],
  },
  "M-10": {
    label: "A speech bubble holding lines of text",
    motifs: ["bubble", "text lines"],
    marks: [p("M6 4h12a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-7l-4 3.5V16H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z"), p("M8 8.25h8M8 11.75h5", "accent")],
  },
  "M-11": {
    label: "A page with one field picked out",
    motifs: ["page", "field"],
    marks: [r(6.5, 12.5, 8, 4, 1, "tint"), ...page(4, 18, 21), p("M7.5 9h7"), r(6.5, 12.5, 8, 4, 1, "accent"), p("M7.5 19h4")],
  },
  "M-12": {
    label: "A network of connected points around a highlighted one",
    motifs: ["graph"],
    marks: [p("M10.2 10.2L7.4 7.4M13.8 10.2l2.8-2.8M10.2 13.8l-2.8 2.8M13.8 13.8l2.8 2.8M8 6h8M8 18h8"), c(6, 6, 2), c(18, 6, 2), c(6, 18, 2), c(18, 18, 2), c(12, 12, 2.5, "dot")],
  },
  "M-13": {
    label: "An eye inside a viewfinder",
    motifs: ["viewfinder", "eye"],
    marks: [p("M3 7.5V4.5a1.5 1.5 0 0 1 1.5-1.5h3M16.5 3h3A1.5 1.5 0 0 1 21 4.5v3M21 16.5v3a1.5 1.5 0 0 1-1.5 1.5h-3M7.5 21h-3A1.5 1.5 0 0 1 3 19.5v-3"), p("M6 12c1.8-3.2 3.8-4.5 6-4.5s4.2 1.3 6 4.5c-1.8 3.2-3.8 4.5-6 4.5S7.8 15.2 6 12z", "accent"), c(12, 12, 1.75, "dot")],
  },
  "M-14": {
    label: "A sound wave",
    motifs: ["waveform"],
    marks: [p("M4 10v4M19.5 10.5v3M7.5 7v10"), p("M11 4v16M14.5 8v8", "accent"), p("M17 9.5v5")],
  },
  "M-15": {
    label: "Three paths from one start, the best-paying one highlighted",
    motifs: ["start", "three options"],
    marks: [c(4, 12, 2), p("M6 12h12M5.75 13c3.5 5.5 7 6 12.25 6"), c(19.75, 12, 1.5), c(19.75, 19, 1.5), p("M5.75 11c3.5-5.5 7-6 12.25-6", "accent"), c(19.75, 5, 1.75, "dot")],
  },
  "M-16": {
    label: "A feasible region with its best corner marked",
    motifs: ["axes", "region", "optimum"],
    marks: [p("M6.5 18V10l4.5-3.5L17 9l2 9z", "tint"), p("M3 3v18h18"), p("M6.5 18V10l4.5-3.5L17 9l2 9z"), p("M13 5.4l8 7.2", "accent", true), c(17, 9, 1.75, "dot")],
  },
  "M-17": {
    label: "A bell curve with its credible interval shaded",
    motifs: ["distribution", "interval"],
    marks: [p("M9 20v-9.5C10 7.6 11 6 12 6s2 1.6 3 4.5V20z", "tint"), p("M3 20.5h18"), p("M3 19.5c4 0 5-13.5 9-13.5s5 13.5 9 13.5"), p("M12 6v14", "accent", true)],
  },
  "M-18": {
    label: "Three layers stacked into one",
    motifs: ["layers"],
    marks: [p("M12 4l8 4-8 4-8-4z", "tint"), p("M12 4l8 4-8 4-8-4z", "accent"), p("M4 12l8 4 8-4M4 16l8 4 8-4")],
  },
  "M-19": {
    label: "A tag with one point labelled among unlabelled ones",
    motifs: ["tag", "few labels"],
    marks: [p("M4 4h7l9 9-7 7-9-9z"), c(8, 8, 1.25, "dot"), c(12.5, 12.5, 1.25, "dot"), c(15, 15, 1, "accent"), c(11, 16, 1, "accent")],
  },
  "M-20": {
    label: "A shield over a grid of records",
    motifs: ["shield", "records"],
    marks: shield([c(10, 10, 1.1, "dot"), c(14, 10, 1.1, "dot"), c(10, 14, 1.1, "dot"), c(14, 14, 1.1, "accent")]),
  },
  "M-21": {
    label: "A stream of data with a loop that keeps the model fresh",
    motifs: ["stream", "refresh loop"],
    marks: [p("M3 7c2-2 4-2 6 0s4 2 6 0M3 12c2-2 4-2 6 0s2.5 1.6 4 1"), p("M20.5 16a4 4 0 1 1-1.2-2.85", "accent"), p("M19.5 10.5v2.9h-2.9", "accent")],
  },
};

/** Orama's solutions (S-… modules, B-… in design), as the engine's catalogue lists them
 * (zetta `src/core/ml/solution_table.cpp`). Each borrows the motif of its primary methodology. */
export const SOLUTION_GLYPHS: Record<string, Glyph> = {
  "S-1": {
    label: "A person, and an arrow turning back towards them",
    motifs: ["person", "return arrow"],
    marks: [...person(8.5, 7.5), p("M15.5 4.5c4.3 1.8 5.6 7.6 2.2 11.2", "accent"), p("M20 16.2l-2.3-.5.4-2.3", "accent")],
  },
  "S-2": {
    label: "A funnel with a star for the best lead",
    motifs: ["funnel", "score"],
    marks: [p("M3 4.5h14l-5 6.5v7l-4 2v-9z"), p("M18.5 12.5l.9 2.6 2.6.9-2.6.9-.9 2.6-.9-2.6-2.6-.9 2.6-.9z", "dot")],
  },
  "S-3": {
    label: "A person beside a growing stack of coins",
    motifs: ["person", "value stack"],
    marks: [r(14, 9.5, 7, 3, 1.5, "tint"), ...person(7, 7.5), r(14, 9.5, 7, 3, 1.5, "accent"), r(14, 13.5, 7, 3, 1.5, "accent"), r(14, 17.5, 7, 3, 1.5, "accent")],
  },
  "S-4": {
    label: "An invoice with its lines in priority order",
    motifs: ["page", "ranked list"],
    marks: [...page(5, 19, 21), p("M8 14.5h6M8 18h3.5"), p("M8 11h8", "accent"), c(16.75, 17.5, 1.25, "dot")],
  },
  "S-5": {
    label: "Past demand as bars, the next periods as dashed bars",
    motifs: ["history bars", "dashed future"],
    marks: [r(13, 11, 3, 9.5, 1, "tint"), r(17.5, 7, 3, 13.5, 1, "tint"), p("M3 20.5h18"), r(4, 13, 3, 7.5, 1), r(8.5, 10, 3, 10.5, 1), r(13, 11, 3, 9.5, 1, "accent", true), r(17.5, 7, 3, 13.5, 1, "accent", true)],
  },
  "S-6": {
    label: "Stacked stock boxes with a refill arrow",
    motifs: ["boxes", "refill arrow"],
    marks: [r(3, 13.5, 6.5, 6.5, 1.25), r(10.5, 13.5, 6.5, 6.5, 1.25), r(6.75, 6, 6.5, 6.5, 1.25), p("M19.75 20V8M17.5 10.25L19.75 8 22 10.25", "accent")],
  },
  "S-7": {
    label: "A magnifying glass over a spike in a line",
    motifs: ["lens", "spike"],
    marks: [c(10, 10, 7), p("M15 15l6 6"), p("M4 12h2.5l2-5.5 2.5 7.5 1.5-2.5H16", "accent")],
  },
  "S-8": {
    label: "A gear with a clock counting down to its service",
    motifs: ["gear", "clock"],
    marks: [c(9, 14, 4.5), c(9, 14, 1.5), p("M9 7.5v1.5M9 19v1.5M2.5 14H4M14 14h1.5M4.4 9.4l1.1 1.1M12.5 17.5l1.1 1.1M4.4 18.6l1.1-1.1M12.5 10.5l1.1-1.1"), c(18, 6, 3.75, "accent"), p("M18 4.25V6h1.5", "accent")],
  },
  "S-9": {
    label: "A paper plane sent out with a trail",
    motifs: ["plane", "trail"],
    marks: [p("M21 3L10.5 13.5l3 7.5z", "tint"), p("M21 3L3 10.5l7.5 3 3 7.5z"), p("M21 3L10.5 13.5", "accent"), p("M3 21l3.5-3.5", "accent", true)],
  },
  "S-10": {
    label: "A shield guarding a small network of accounts",
    motifs: ["shield", "graph"],
    marks: shield([p("M11.3 9.3l-1.6 3.4M12.7 9.3l1.6 3.4M10.6 14h2.8", "accent"), c(12, 8.25, 1.3, "dot"), c(9.25, 14, 1.3, "dot"), c(14.75, 14, 1.3, "dot")]),
  },
  "S-11": {
    label: "A person and an arrow to the centre of a target",
    motifs: ["person", "target"],
    marks: [...person(6.5, 11.5), c(17.5, 8, 4), p("M10.5 13c2.3-2.3 4-3.6 5.8-4.4", "accent"), c(17.5, 8, 1.6, "dot")],
  },
  "S-12": {
    label: "A claim splitting into three paths, one referred",
    motifs: ["page", "three options"],
    marks: [...page(3, 13, 21), p("M6 11h4M6 14.5h3"), p("M13 12h1.5M14.5 12c1.5 0 2-6 4.5-6M14.5 12H19M14.5 12c1.5 0 2 6 4.5 6"), c(20.5, 6, 1.5), c(20.5, 12, 1.5), c(20.5, 18, 1.75, "dot")],
  },
  "B-1": {
    label: "A price tag with a bounded up and down range",
    motifs: ["tag", "band"],
    marks: [p("M3 4.5A1.5 1.5 0 0 1 4.5 3H11l8 8-8 8-8-8z"), c(7, 7, 1.25, "dot"), p("M20 13.5v7.5M18 15.5l2-2 2 2M18 19l2 2 2-2", "accent")],
  },
  "B-2": {
    label: "A group of people counted together",
    motifs: ["person", "group"],
    marks: [r(2.5, 3, 19, 18, 3, "accent", true), c(12, 8.5, 2.25), p("M7.5 18c0-2.7 2-4.5 4.5-4.5s4.5 1.8 4.5 4.5"), c(6.5, 10.5, 1.5, "dot"), c(17.5, 10.5, 1.5, "dot")],
  },
  "B-3": {
    label: "A ticket routed two ways",
    motifs: ["ticket", "routing"],
    marks: [p("M3 7h11v3a2 2 0 0 0 0 4v3H3v-3a2 2 0 0 0 0-4z"), p("M14 12h2.5M16.5 12L20 8M16.5 12l3.5 4M18 8h2v2M18 16h2v-2", "accent")],
  },
  "B-4": {
    label: "A page feeding an automated workflow",
    motifs: ["page", "flow"],
    marks: [r(16, 4, 5, 5, 1.25, "tint"), ...page(3, 13, 21), p("M6 11h4M6 14.5h3"), p("M13 12h1.5V6.5H16M14.5 12v5.5H16", "accent"), r(16, 4, 5, 5, 1.25, "accent"), r(16, 15, 5, 5, 1.25, "accent")],
  },
  "B-5": {
    label: "A factory with a warning",
    motifs: ["factory", "warning"],
    marks: [p("M3 20V11l4 2.5V11l4 2.5V5h3v15zM2.5 20h12"), p("M18.5 12.5l3.3 6.5h-6.6z", "accent"), p("M18.5 15v1.5", "accent")],
  },
  "B-6": {
    label: "A calendar with a medical cross",
    motifs: ["calendar", "cross"],
    marks: [r(3, 5, 18, 16, 2), p("M3 10h18M8 3v4M16 3v4"), p("M12 12.5v6M9 15.5h6", "accent")],
  },
  "B-7": {
    label: "A credit card above a limit gauge",
    motifs: ["card", "gauge"],
    marks: [r(3, 3.5, 14, 9.5, 1.5), p("M3 7h14"), p("M11.5 19.5a4.5 4.5 0 0 1 9 0", "accent"), p("M16 19.5l2.4-2.9", "accent"), c(16, 19.5, 1.25, "dot")],
  },
  "B-8": {
    label: "Servers guarded by a shield",
    motifs: ["servers", "shield"],
    marks: [p("M18.5 11l3.5 1.5v3c0 2.5-1.5 4.3-3.5 5.5-2-1.2-3.5-3-3.5-5.5v-3z", "tint"), r(3, 4, 10, 5, 1.25), r(3, 11, 10, 5, 1.25), c(6, 6.5, 0.9, "dot"), c(6, 13.5, 0.9, "dot"), p("M3 18.5h7"), p("M18.5 11l3.5 1.5v3c0 2.5-1.5 4.3-3.5 5.5-2-1.2-3.5-3-3.5-5.5v-3z", "accent")],
  },
};

/** The methodology families the engine groups by, each with a glyph for a methodology the set has
 * not drawn yet. */
export const FAMILY_GLYPHS: Record<string, Glyph> = {
  tabular: {
    label: "Four cells of a table, one highlighted",
    motifs: ["table"],
    marks: [r(13, 13, 7, 7, 1.5, "tint"), r(4, 4, 7, 7, 1.5), r(13, 4, 7, 7, 1.5), r(4, 13, 7, 7, 1.5), r(13, 13, 7, 7, 1.5, "accent")],
  },
  identity: {
    label: "A tree of linked records",
    motifs: ["graph"],
    marks: [p("M12 7v4H6v5M12 11h6v5"), c(6, 18, 2), c(18, 18, 2), c(12, 5, 2, "dot")],
  },
  time: {
    label: "A clock face",
    motifs: ["clock"],
    marks: [c(12, 12, 8.5), p("M12 7v5l3.5 2", "accent")],
  },
  action: {
    label: "A signpost choosing a direction",
    motifs: ["signpost"],
    marks: [p("M12 21V3M12 11H6l-2 2 2 2h6"), p("M12 5h6l2 2-2 2h-6", "accent")],
  },
  language: {
    label: "A speech bubble",
    motifs: ["bubble"],
    marks: [p("M6 4h12a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-7l-4 3.5V16H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z"), c(8.5, 10, 1.1, "dot"), c(12, 10, 1.1, "dot"), c(15.5, 10, 1.1, "dot")],
  },
  data: {
    label: "A database",
    motifs: ["store"],
    marks: [p("M5 6c0-1.7 3.1-3 7-3s7 1.3 7 3-3.1 3-7 3-7-1.3-7-3zM5 6v12c0 1.7 3.1 3 7 3s7-1.3 7-3V6"), p("M5 12c0 1.7 3.1 3 7 3s7-1.3 7-3", "accent")],
  },
};

/** The family glyph for an engine family name ("Time & vigilance", …), or null. */
export function familyKey(family: string): keyof typeof FAMILY_GLYPHS | null {
  const f = family.toLowerCase();
  if (f.includes("tabular")) return "tabular";
  if (f.includes("identity") || f.includes("structure")) return "identity";
  if (f.includes("time") || f.includes("vigilance")) return "time";
  if (f.includes("action") || f.includes("decision")) return "action";
  if (f.includes("language") || f.includes("document") || f.includes("perception")) return "language";
  if (f.includes("data") || f.includes("supply")) return "data";
  return null;
}

/** What an icon draws: a glyph (and where it came from) or a monogram, with its accessible label. */
export type IconChoice =
  | { glyph: Glyph; from: "catalogue" | "duplicate" | "methodology" | "family"; key: string; label: string }
  | { monogram: string; from: "monogram"; key: string; label: string };

type Entry = Record<string, unknown> | null | undefined;

const str = (entry: Entry, key: string): string => {
  const v = entry?.[key];
  return typeof v === "string" ? v.trim() : "";
};

/** The first letter or digit of a name, upper-cased, or "?". */
export function monogramOf(name: string): string {
  const m = /[\p{L}\p{N}]/u.exec(name);
  return m ? m[0].toLocaleUpperCase() : "?";
}

/** A solution's primary methodology: the one marked primary, else the first it lists. */
export function primaryMethodology(solution: Entry): string {
  const list = Array.isArray(solution?.methodologies) ? solution!.methodologies : Array.isArray(solution?.composition) ? solution!.composition : [];
  let first = "";
  for (const item of list as unknown[]) {
    const id = typeof item === "string" ? item : item && typeof item === "object" ? str(item as Entry, "methodology") || str(item as Entry, "id") : "";
    if (!id) continue;
    if (!first) first = id;
    if (item && typeof item === "object" && str(item as Entry, "role") === "primary") return id;
  }
  return first;
}

/** The icon of a methodology: its own glyph, else its family's, else a monogram. */
export function methodologyIcon(entry: Entry): IconChoice {
  const id = str(entry, "id");
  const name = str(entry, "name") || id;
  const own = METHODOLOGY_GLYPHS[id];
  if (own) return { glyph: own, from: "catalogue", key: id, label: own.label };
  const family = familyKey(str(entry, "family"));
  if (family) return { glyph: FAMILY_GLYPHS[family], from: "family", key: `family:${family}`, label: FAMILY_GLYPHS[family].label };
  return { monogram: monogramOf(name), from: "monogram", key: `monogram:${monogramOf(name)}`, label: `The letter ${monogramOf(name)}` };
}

/** The icon of a solution: Orama's own glyph; a duplicate's original's; else the glyph of the
 * methodology it is built on (`families` names the family of a methodology the set has not drawn,
 * from the engine's list); else a monogram of its name. */
export function solutionIcon(entry: Entry, families: Record<string, string> = {}): IconChoice {
  const id = str(entry, "id");
  const name = str(entry, "name") || id;
  const own = SOLUTION_GLYPHS[id];
  if (own) return { glyph: own, from: "catalogue", key: id, label: own.label };
  const original = SOLUTION_GLYPHS[str(entry, "duplicated_from")];
  if (original) return { glyph: original, from: "duplicate", key: str(entry, "duplicated_from"), label: original.label };
  const primary = primaryMethodology(entry);
  if (primary) {
    const chosen = methodologyIcon({ id: primary, family: families[primary] ?? "" });
    if (chosen.from !== "monogram") return { ...chosen, from: chosen.from === "catalogue" ? "methodology" : chosen.from };
  }
  const letter = monogramOf(name);
  return { monogram: letter, from: "monogram", key: `monogram:${letter}`, label: `The letter ${letter}` };
}

/** The catalogue's names as the engine lists them (zetta dev 0ae708dc), for the gallery, the
 * website export and the test that every catalogue entry has its own glyph. */
export const CATALOGUE_NAMES: Record<string, string> = {
  "M-1": "Tabular supervised learning",
  "M-2": "Entity resolution",
  "M-3": "Segmentation / clustering",
  "M-4": "Time-series forecasting",
  "M-5": "Survival / time-to-event",
  "M-6": "Causal inference & uplift",
  "M-7": "Anomaly detection",
  "M-8": "Recommendation / next-best-action",
  "M-9": "Learning-to-rank",
  "M-10": "NLP / embeddings / RAG / fine-tuning",
  "M-11": "Document intelligence",
  "M-12": "Graph ML",
  "M-13": "Computer vision",
  "M-14": "Speech & audio analytics",
  "M-15": "Bandits & sequential decisioning",
  "M-16": "Optimization / operations research",
  "M-17": "Bayesian / probabilistic modeling & uncertainty quantification",
  "M-18": "Ensembling & stacking",
  "M-19": "Label efficiency",
  "M-20": "Privacy-preserving learning & synthetic data",
  "M-21": "Online / streaming learning",
  "S-1": "Churn & Retention",
  "S-2": "Lead & Opportunity Scoring",
  "S-3": "Customer Lifetime Value",
  "S-4": "Collections Prioritization",
  "S-5": "Demand Forecasting",
  "S-6": "Replenishment & Inventory Action",
  "S-7": "KPI Anomaly & Driver Analysis",
  "S-8": "Predictive Maintenance",
  "S-9": "Automated Outbound",
  "S-10": "Fraud & Financial-Crime Decisioning",
  "S-11": "Next-Best-Action",
  "S-12": "Claims Triage & Fraud Referral",
  "B-1": "Dynamic Pricing & Discount Governance",
  "B-2": "Employee Attrition (aggregate-first)",
  "B-3": "Ticket Triage & Service Automation",
  "B-4": "Document & Workflow Automation",
  "B-5": "Supplier Risk & Procurement Intelligence",
  "B-6": "Healthcare Operations (no-show & denials)",
  "B-7": "Credit Underwriting & Limit Management",
  "B-8": "IT & Security Operations",
};

/** The family names the engine groups methodologies by, with the family glyph each takes. */
export const ENGINE_FAMILIES: Record<string, keyof typeof FAMILY_GLYPHS> = {
  "The tabular decision core": "tabular",
  "Identity & structure": "identity",
  "Time & vigilance": "time",
  "Action & decision": "action",
  "Language, documents & perception": "language",
  "The data supply chain": "data",
};

const ESCAPES: Record<string, string> = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" };
const escapeXml = (s: string) => s.replace(/[&<>"]/g, (ch) => ESCAPES[ch]);

/** The accent each kind takes outside lake (the light theme's token values), for the website. */
export const STANDALONE_ACCENT = { methodology: "#0f62fe", solution: "#8e6a00" } as const;

/** A glyph as a standalone SVG file (the website's copy, `tools/ui/export-catalogue-icons.mjs`):
 * the same marks, styled by classes whose colours are CSS custom properties (`--cat-accent`,
 * `--cat-tint`) with the light theme's values as fallbacks, and the ink as `currentColor`. */
export function glyphSvg(glyph: Glyph, kind: "methodology" | "solution", title: string): string {
  const accent = STANDALONE_ACCENT[kind];
  const cls = (m: Mark) => ({ line: "l", accent: "a", dot: "d", tint: "t" })[m.tone] + (m.dash ? " s" : "");
  const shape = (m: Mark) =>
    m.kind === "path"
      ? `<path class="${cls(m)}" d="${m.d}"/>`
      : m.kind === "circle"
        ? `<circle class="${cls(m)}" cx="${m.cx}" cy="${m.cy}" r="${m.r}"/>`
        : `<rect class="${cls(m)}" x="${m.x}" y="${m.y}" width="${m.w}" height="${m.h}" rx="${m.rx}"/>`;
  return [
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24" role="img" aria-label="${escapeXml(glyph.label)}">`,
    `<title>${escapeXml(title)}</title>`,
    `<style>.l,.a{fill:none;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}.l{stroke:currentColor}.a{stroke:var(--cat-accent,${accent})}.d{fill:var(--cat-accent,${accent})}.t{fill:var(--cat-tint,${accent}33)}.s{stroke-dasharray:2 2.5}</style>`,
    ...glyph.marks.map(shape),
    "</svg>",
    "",
  ].join("\n");
}

/** Every file the website export writes: `<kind>/<id>.svg` → its contents. */
export function standaloneFiles(): Record<string, string> {
  const files: Record<string, string> = {};
  for (const [id, glyph] of Object.entries(METHODOLOGY_GLYPHS)) files[`methodologies/${id}.svg`] = glyphSvg(glyph, "methodology", `${id} ${CATALOGUE_NAMES[id] ?? ""}`.trim());
  for (const [id, glyph] of Object.entries(SOLUTION_GLYPHS)) files[`solutions/${id}.svg`] = glyphSvg(glyph, "solution", `${id} ${CATALOGUE_NAMES[id] ?? ""}`.trim());
  for (const [family, key] of Object.entries(ENGINE_FAMILIES)) files[`families/${key}.svg`] = glyphSvg(FAMILY_GLYPHS[key], "methodology", family);
  return files;
}
