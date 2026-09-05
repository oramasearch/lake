#!/usr/bin/env node

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const css = readFileSync(resolve(root, "src/api/web_ui_tokens.inc"), "utf8");

function block(selector, from = 0) {
  const start = css.indexOf(selector, from);
  if (start < 0) {
    throw new Error(`missing selector ${selector}`);
  }
  const open = css.indexOf("{", start);
  const close = css.indexOf("}", open);
  return css.slice(open + 1, close);
}

function declarations(source) {
  const values = new Map();
  for (const match of source.matchAll(/(--[\w-]+):\s*([^;]+);/g)) {
    values.set(match[1], match[2].trim());
  }
  return values;
}

const base = declarations(block(":root {"));
const light = new Map([...base, ...declarations(block(':root, [data-theme="light"]'))]);
const dark = new Map([...base, ...declarations(block(':root[data-theme="dark"]'))]);

function color(values, name, seen = new Set()) {
  if (seen.has(name)) {
    throw new Error(`cyclic token ${name}`);
  }
  seen.add(name);
  const value = values.get(name);
  if (!value) {
    throw new Error(`missing token ${name}`);
  }
  const reference = value.match(/^var\((--[\w-]+)\)$/);
  if (reference) {
    return color(values, reference[1], seen);
  }
  if (!/^#[0-9a-f]{6}$/i.test(value)) {
    throw new Error(`token ${name} is not an opaque color: ${value}`);
  }
  return value.toLowerCase();
}

function luminance(hex) {
  const channels = [1, 3, 5].map((offset) => Number.parseInt(hex.slice(offset, offset + 2), 16) / 255);
  const linear = channels.map((channel) =>
    channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4,
  );
  return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2];
}

function ratio(first, second) {
  const lighter = Math.max(luminance(first), luminance(second));
  const darker = Math.min(luminance(first), luminance(second));
  return (lighter + 0.05) / (darker + 0.05);
}

const pairs = [
  ["primary/canvas", "--lake-text-primary", "--lake-canvas", 4.5],
  ["secondary/canvas", "--lake-text-secondary", "--lake-canvas", 4.5],
  ["tertiary/canvas", "--lake-text-tertiary", "--lake-canvas", 4.5],
  ["primary/surface", "--lake-text-primary", "--lake-surface", 4.5],
  ["action/canvas", "--lake-action", "--lake-canvas", 4.5],
  ["on-action/action", "--lake-text-on-action", "--lake-action-filled", 4.5],
  ["success/soft", "--lake-success", "--lake-success-soft", 4.5],
  ["warning/soft", "--lake-warning", "--lake-warning-soft", 4.5],
  ["danger/soft", "--lake-danger", "--lake-danger-soft", 4.5],
  ["ai/soft", "--lake-ai", "--lake-ai-soft", 4.5],
  ["focus/canvas", "--lake-focus", "--lake-canvas", 3],
  ["border/surface", "--lake-border-strong", "--lake-surface", 3],
];

let failed = false;
console.log("theme  pair                 foreground  background  ratio  min  result");
for (const [theme, values] of [
  ["light", light],
  ["dark", dark],
]) {
  for (const [name, foregroundName, backgroundName, minimum] of pairs) {
    const foreground = color(values, foregroundName);
    const background = color(values, backgroundName);
    const measured = ratio(foreground, background);
    const passed = measured + Number.EPSILON >= minimum;
    failed ||= !passed;
    console.log(
      `${theme.padEnd(6)} ${name.padEnd(20)} ${foreground.padEnd(11)} ${background.padEnd(11)} ` +
        `${measured.toFixed(2).padStart(5)}  ${minimum.toFixed(1)}  ${passed ? "PASS" : "FAIL"}`,
    );
  }
}
if (failed) {
  process.exitCode = 1;
}
