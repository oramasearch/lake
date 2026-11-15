#!/usr/bin/env node
// Writes the catalogue icons (lake#639) as standalone SVG files under assets/catalogue-icons/, the
// copy a website can use. The source of record is
// frontend/src/lib/catalogueIcons.ts; never edit the SVG files by hand.
//
//   node tools/ui/export-catalogue-icons.mjs            write the files
//   node tools/ui/export-catalogue-icons.mjs --check    fail if a file is missing, stale or extra
//   node tools/ui/export-catalogue-icons.mjs --preview <file.html>   also write a review page
//
// Needs Node 22.6 or later (it imports the TypeScript source with type stripping).
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../..", import.meta.url));
const OUT = join(ROOT, "assets", "catalogue-icons");
const { standaloneFiles } = await import(join(ROOT, "frontend", "src", "lib", "catalogueIcons.ts"));

const args = process.argv.slice(2);
const check = args.includes("--check");
const previewAt = args.includes("--preview") ? args[args.indexOf("--preview") + 1] : "";
const files = standaloneFiles();

function existing(dir) {
  if (!existsSync(dir)) return [];
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? existing(join(dir, e.name)) : e.name.endsWith(".svg") ? [join(dir, e.name)] : []));
}

const problems = [];
for (const [path, body] of Object.entries(files)) {
  const target = join(OUT, path);
  if (check) {
    if (!existsSync(target)) problems.push(`missing ${path}`);
    else if (readFileSync(target, "utf8") !== body) problems.push(`stale ${path}`);
  } else {
    mkdirSync(dirname(target), { recursive: true });
    writeFileSync(target, body);
  }
}
for (const file of existing(OUT)) {
  const path = relative(OUT, file);
  if (!(path in files)) {
    if (check) problems.push(`extra ${path}`);
    else rmSync(file);
  }
}
if (problems.length) {
  console.error(`export-catalogue-icons: ${problems.join(", ")}; run node tools/ui/export-catalogue-icons.mjs`);
  process.exit(1);
}

if (previewAt) {
  const cell = (path, body) =>
    `<figure><div class="sizes">${[20, 32, 64].map((s) => `<span class="tile" style="width:${s}px;height:${s}px">${body.replace('width="24" height="24"', `width="${Math.round(s * (s >= 28 ? 0.6667 : 1))}" height="${Math.round(s * (s >= 28 ? 0.6667 : 1))}"`)}</span>`).join("")}</div><figcaption>${path}</figcaption></figure>`;
  const html = `<!doctype html><meta charset="utf-8"><title>Catalogue icons</title><style>
body{font:13px system-ui;margin:24px;background:#f8f8fa;color:#161616;--cat-accent:#0f62fe}
.sol{--cat-accent:#8e6a00}
.grid>div{--cat-tint:color-mix(in srgb,var(--cat-accent) 20%,transparent)}
body.dark{background:#0d0d10;color:#f7f7fa}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}
figure{margin:0;padding:10px;border:1px solid #8883;border-radius:6px}
.sizes{display:flex;gap:10px;align-items:center}
.tile{display:inline-flex;align-items:center;justify-content:center}
.tile:not(:first-child){background:#f1edff;border-radius:25%}
body.dark .tile:not(:first-child){background:#2b234f}
body.dark{--cat-accent:#4c82ff}
body.dark .sol{--cat-accent:#f4c84b}
figcaption{margin-top:6px;font-family:monospace}
</style>
<label><input type="checkbox" onchange="document.body.classList.toggle('dark',this.checked)"> dark</label>
<div class="grid">${Object.entries(files)
    .map(([path, body]) => `<div class="${path.startsWith("solutions") ? "sol" : ""}">${cell(path, body)}</div>`)
    .join("")}</div>`;
  writeFileSync(previewAt, html);
}
console.log(`export-catalogue-icons: ${check ? "checked" : "wrote"} ${Object.keys(files).length} files`);
