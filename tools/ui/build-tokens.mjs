#!/usr/bin/env node

import { existsSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../..", import.meta.url));
const DTCG_ROOT = join(ROOT, "vendor", "orama-design", "tokens", "dtcg");
const MAP_PATH = join(ROOT, "design-tokens.map.json");
const INVENTORY_PATH = join(ROOT, "design-tokens.inventory.json");
const OUTPUT_PATH = join(ROOT, "src", "api", "web_ui_tokens.inc");
const ALIAS = /^\{([^}]+)\}$/;

function fail(message) {
  console.error(`build-tokens: ${message}`);
  process.exit(1);
}

function parseArguments(argv) {
  const parsed = { check: false, checkFile: "" };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--check") {
      parsed.check = true;
      continue;
    }
    if (argument === "--check-file") {
      const value = argv[index + 1];
      if (!value) {
        fail("--check-file requires a path");
      }
      parsed.check = true;
      parsed.checkFile = resolve(value);
      index += 1;
      continue;
    }
    fail(`unrecognized argument: ${argument}`);
  }
  return parsed;
}

function tokenFiles(directory) {
  const files = [];
  function visit(current) {
    for (const entry of readdirSync(current, { withFileTypes: true }).sort((left, right) =>
      left.name.localeCompare(right.name),
    )) {
      const path = join(current, entry.name);
      if (entry.isDirectory()) {
        visit(path);
      } else if (entry.name.endsWith(".tokens.json")) {
        files.push(path);
      }
    }
  }
  visit(directory);
  return files;
}

function flatten(document, source) {
  const tokens = new Map();
  function visit(node, prefix, inheritedType) {
    const type = node.$type ?? inheritedType;
    for (const [key, child] of Object.entries(node)) {
      if (key.startsWith("$")) {
        continue;
      }
      const path = prefix ? `${prefix}.${key}` : key;
      if (child && typeof child === "object" && "$value" in child) {
        tokens.set(path, {
          path,
          type: child.$type ?? type,
          value: child.$value,
          dark: child.$extensions?.["orama.dark"],
          source,
        });
      } else if (child && typeof child === "object") {
        visit(child, path, type);
      }
    }
  }
  visit(document, "", undefined);
  return tokens;
}

function cssName(path) {
  return `--orama-${path.replaceAll(".", "-")}`;
}

function dimension(value, path) {
  if (!value.endsWith("px") || path.startsWith("border.") || path === "radius.full") {
    return value;
  }
  const pixels = Number.parseFloat(value);
  if (pixels === 0) {
    return "0";
  }
  return `${Number((pixels / 16).toFixed(6))}rem`;
}

function terminalAlias(alias, tokens, seen = []) {
  const path = alias.match(ALIAS)?.[1];
  if (!path || !tokens.has(path)) {
    fail(`unresolved alias ${alias}`);
  }
  if (seen.includes(path)) {
    fail(`alias cycle: ${[...seen, path].join(" -> ")}`);
  }
  const token = tokens.get(path);
  if (typeof token.value === "string" && ALIAS.test(token.value)) {
    return terminalAlias(token.value, tokens, [...seen, path]);
  }
  return path;
}

function cssValue(value, type, path, tokens) {
  if (typeof value === "string" && ALIAS.test(value)) {
    return `var(${cssName(terminalAlias(value, tokens))})`;
  }
  if (type === "fontFamily") {
    return value
      .map((family) => (/^(sans-serif|monospace|serif)$/.test(family) ? family : `'${family}'`))
      .join(", ");
  }
  if (type === "cubicBezier") {
    return `cubic-bezier(${value.join(", ")})`;
  }
  if (type === "dimension") {
    return dimension(value, path);
  }
  if (type === "shadow") {
    return `${dimension(value.offsetX, path)} ${dimension(value.offsetY, path)} ${dimension(
      value.blur,
      path,
    )} ${dimension(value.spread, path)} ${value.color}`;
  }
  return String(value);
}

function declarations(entries, valueFor) {
  return entries.map((token) => `  ${cssName(token.path)}: ${valueFor(token)};`);
}

function compile() {
  const tokens = new Map();
  for (const file of tokenFiles(DTCG_ROOT)) {
    const document = JSON.parse(readFileSync(file, "utf8"));
    for (const [path, token] of flatten(document, relative(DTCG_ROOT, file))) {
      if (tokens.has(path)) {
        fail(`duplicate DTCG path ${path}`);
      }
      tokens.set(path, token);
    }
  }

  const mapping = JSON.parse(readFileSync(MAP_PATH, "utf8"));
  const inventory = JSON.parse(readFileSync(INVENTORY_PATH, "utf8")).lakeSemantics;
  const mappedNames = Object.keys(mapping.roles).sort();
  const expectedNames = [...inventory].sort();
  if (JSON.stringify(mappedNames) !== JSON.stringify(expectedNames)) {
    fail("design-tokens.map.json and design-tokens.inventory.json differ");
  }
  for (const [name, path] of Object.entries(mapping.roles)) {
    if (!name.startsWith("--lake-") || !tokens.has(path)) {
      fail(`invalid map entry ${name} -> ${path}`);
    }
  }

  const all = [...tokens.values()];
  const primitives = all.filter((token) => !token.value?.toString().match?.(ALIAS));
  const aliases = all.filter((token) => token.value?.toString().match?.(ALIAS));
  const themed = all.filter((token) => token.dark);
  const unthemedAliases = aliases.filter((token) => !token.dark);
  const lines = [
    "/* Generated by tools/ui/build-tokens.mjs. Do not edit. */",
    ":root {",
    ...declarations(primitives, (token) => cssValue(token.value, token.type, token.path, tokens)),
    ...declarations(unthemedAliases, (token) =>
      cssValue(token.value, token.type, token.path, tokens),
    ),
    ...Object.entries(mapping.roles).map(
      ([name, path]) => `  ${name}: var(${cssName(path)});`,
    ),
    "}",
    "",
    ':root, [data-theme="light"] {',
    "  color-scheme: light;",
    ...declarations(themed, (token) => cssValue(token.value, token.type, token.path, tokens)),
    "}",
    "",
    '@media (prefers-color-scheme: dark) {',
    '  :root:not([data-theme="light"]) {',
    "    color-scheme: dark;",
    ...declarations(themed, (token) =>
      cssValue(token.dark, token.type, token.path, tokens).replace(/^  /, "    "),
    ).map((line) => `  ${line}`),
    "  }",
    "}",
    "",
    ':root[data-theme="dark"] {',
    "  color-scheme: dark;",
    ...declarations(themed, (token) => cssValue(token.dark, token.type, token.path, tokens)),
    "}",
    "",
    "@media (prefers-reduced-motion: reduce) {",
    "  :root {",
    "    --orama-duration-fast: 0ms;",
    "    --orama-duration-standard: 0ms;",
    "    --orama-duration-deliberate: 0ms;",
    "  }",
    "}",
  ];
  const css = `${lines.join("\n")}\n`;
  return `R"lake_tokens(\n${css})lake_tokens"`;
}

const arguments_ = parseArguments(process.argv.slice(2));
const generated = compile();
if (arguments_.check) {
  const checkedPath = arguments_.checkFile || OUTPUT_PATH;
  if (!existsSync(checkedPath) || readFileSync(checkedPath, "utf8") !== generated) {
    fail(`${relative(ROOT, checkedPath)} does not match a fresh compile`);
  }
  console.log(`build-tokens: ok — ${relative(ROOT, checkedPath)} matches the vendored tokens`);
} else {
  writeFileSync(OUTPUT_PATH, generated);
  console.log(`build-tokens: wrote ${relative(ROOT, OUTPUT_PATH)}`);
}
