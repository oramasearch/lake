/**
 * The Docs reader (src/pages/developer/docs.ts, lake#561): pages grouped by section, the instant
 * filter over titles and headings, and the addresses of pages and headings.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { docHref, type DocPage, filterDocs, groupBySection, splitAround, tableOfContents } from "../src/pages/developer/docs.ts";

const page = (slug: string, title: string, section: string, headings: string[] = []): DocPage => ({
  slug,
  title,
  section,
  headings: headings.map((text) => ({ level: 2, text, anchor: text.toLowerCase().replace(/\s+/g, "-") })),
});

const pages = [
  page("index", "Getting Started", "Get Started", ["Start Your Workspace"]),
  page("concepts", "Concepts and Glossary", "Get Started"),
  page("repositories", "Repositories", "Data", ["Push and Pull"]),
  page("repository-settings", "Branch Protection, Webhooks, Import and Export", "Data", ["Protect a Branch"]),
  page("train", "Train a Model", "Train", ["Read the Report"]),
];

test("pages are grouped in the sections' order, each section once", () => {
  const grouped = groupBySection(pages, ["Get Started", "Data", "Train"]);
  assert.deepEqual(
    grouped.map((s) => [s.name, s.pages.map((p) => p.slug)]),
    [
      ["Get Started", ["index", "concepts"]],
      ["Data", ["repositories", "repository-settings"]],
      ["Train", ["train"]],
    ],
  );
});

test("a lake that answers no sections still groups its pages in reading order", () => {
  assert.deepEqual(
    groupBySection(pages).map((s) => s.name),
    ["Get Started", "Data", "Train"],
  );
  assert.deepEqual(groupBySection([{ slug: "a", title: "A", section: "", headings: [] }]).map((s) => s.name), ["Docs"]);
});

test("the instant filter finds titles first, then headings, every word anywhere", () => {
  const hits = filterDocs(pages, "branch protect");
  assert.equal(hits[0].slug, "repository-settings");
  assert.equal(hits[0].heading, undefined);
  assert.deepEqual(
    filterDocs(pages, "PUSH").map((h) => [h.slug, h.heading?.anchor]),
    [["repositories", "push-and-pull"]],
  );
  assert.deepEqual(filterDocs(pages, "   "), []);
  assert.equal(filterDocs(pages, "e", 2).length, 2);
});

test("the first guide is /docs, others are /docs/<slug>, headings are #anchors", () => {
  assert.equal(docHref("index", "index"), "/docs");
  assert.equal(docHref("train", "index"), "/docs/train");
  assert.equal(docHref("train", "index", "read-the-report"), "/docs/train#read-the-report");
  assert.equal(docHref("index", "index", "start"), "/docs#start");
});

test("on this page lists level 2 and 3 headings", () => {
  const toc = tableOfContents([
    { level: 2, text: "A", anchor: "a" },
    { level: 3, text: "B", anchor: "b" },
    { level: 4, text: "C", anchor: "c" },
  ]);
  assert.deepEqual(toc.map((h) => h.anchor), ["a", "b"]);
});

test("a match is split for highlighting, case ignored", () => {
  assert.deepEqual(splitAround("Read the Report", "report"), ["Read the ", "Report", ""]);
  assert.equal(splitAround("Read", "x"), null);
  assert.equal(splitAround("Read", " "), null);
});
