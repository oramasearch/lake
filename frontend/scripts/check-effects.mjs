// Effects return nothing or a cleanup function. React calls whatever an effect returns as
// its cleanup the next time the effect runs; an arrow with an expression body returns that
// expression's value — and newer browsers return a Promise from a smooth `scrollIntoView`, so
// `useEffect(() => el.scrollIntoView(...), [dep])` crashed the Assistant screen with
// "is not a function" the second time it ran. Every effect callback is a block body: `() => { … }`.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const effect = /\buse(?:Layout|Insertion)?Effect\(\s*(?:async\s*)?\([^)]*\)\s*=>(?!\s*\{)/g;

/** The expression-bodied effects in a source text, as [line, excerpt]. */
export function expressionEffects(text) {
  return [...text.matchAll(effect)].map((m) => [text.slice(0, m.index).split("\n").length, text.slice(m.index, m.index + 70).split("\n")[0]]);
}

function walk(dir, found) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) walk(path, found);
    else if (/\.(tsx?|jsx?)$/.test(name)) {
      for (const [line, excerpt] of expressionEffects(readFileSync(path, "utf8"))) found.push(`${path}:${line}: ${excerpt}`);
    }
  }
  return found;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const found = walk("src", []);
  if (found.length) {
    console.error(`Effects must have a block body so they return nothing or a cleanup:\n  ${found.join("\n  ")}`);
    process.exit(1);
  }
  console.log("every effect has a block body");
}
