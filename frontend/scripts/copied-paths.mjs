// SPDX-License-Identifier: MIT

// Which paths outside frontend/ the image's `frontend` stage can read (lake#693).
//
// The Dockerfile's `frontend` stage builds the app in /frontend from `COPY frontend/ ./` plus
// whatever else it names, and runs `npm run build` (the tests included) there. A test that reads a
// file beside frontend/ passes in a checkout and fails only in the image build (lake#639 read
// assets/catalogue-icons/, lake#653 src/api/engine_openapi.json). The stage's working directory is
// /frontend, so a checkout path <repo>/<p> is read in the image as /<p>: a read is covered when a
// COPY in the stage puts <p> at /<p>.
//
// Pure functions only: the preload in watch-copied-reads.mjs records the reads and calls these.
import { posix } from "node:path";

const STAGE = /^FROM\s+\S+\s+AS\s+(\S+)\s*$/i;

/** The COPY instructions of the Dockerfile's `frontend` stage, with where each puts its sources. */
export function frontendCopies(dockerfile, stage = "frontend") {
  const lines = dockerfile.split("\n");
  const copies = [];
  let inside = false;
  let workdir = "/";
  let start = 0;
  let lastCopy = 0;
  let found = false;
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i].trim();
    if (/^FROM\s/i.test(line)) {
      const match = STAGE.exec(line);
      inside = !!match && match[1].toLowerCase() === stage;
      if (inside) {
        found = true;
        start = i + 1;
        workdir = "/";
      }
      continue;
    }
    if (!inside) continue;
    const work = /^WORKDIR\s+(\S+)/i.exec(line);
    if (work) workdir = posix.resolve(workdir, work[1]);
    const copy = /^COPY\s+(.*)$/i.exec(line);
    if (!copy) continue;
    lastCopy = i + 1;
    const words = copy[1].split(/\s+/).filter(Boolean);
    if (words.some((w) => w.startsWith("--from"))) continue;
    const args = words.filter((w) => !w.startsWith("--"));
    if (args.length < 2) continue;
    const rawDest = args[args.length - 1];
    const dest = posix.resolve(workdir, rawDest);
    const intoDirectory = rawDest.endsWith("/") || args.length > 2;
    for (const source of args.slice(0, -1)) {
      copies.push({ line: i + 1, text: line, source: source.replace(/^\.\//, "").replace(/\/+$/, ""), dest, intoDirectory });
    }
  }
  return { found, start, insertAt: lastCopy, copies };
}

/** Where `path` (relative to the repository) lands in the image, if a copy carries it; else null. */
export function imagePathOf(path, copies, isDirectory) {
  for (const copy of copies) {
    if (copy.source === "" || copy.source === ".") return posix.join(copy.dest, path);
    if (path === copy.source) {
      if (isDirectory(copy.source)) return copy.dest;
      return copy.intoDirectory ? posix.join(copy.dest, posix.basename(path)) : copy.dest;
    }
    if (path.startsWith(`${copy.source}/`) && isDirectory(copy.source)) {
      return posix.join(copy.dest, path.slice(copy.source.length + 1));
    }
  }
  return null;
}

/** The line that would copy `path` to where the stage's tests read it. */
export function copyLineFor(path, isDirectory) {
  return isDirectory ? `COPY ${path}/ /${path}/` : `COPY ${path} /${path}`;
}

/**
 * The reads the image cannot satisfy. `reads` are repository-relative paths outside frontend/
 * (each `{ path, by }`), `isDirectory(path)` answers for the checkout. A read under a directory
 * that is itself reported is folded into it, so one walk of a folder asks for one line.
 */
export function uncopiedReads(reads, copies, isDirectory) {
  const missing = new Map();
  for (const { path, by } of reads) {
    if (imagePathOf(path, copies, isDirectory) === `/${path}`) continue;
    if (!missing.has(path)) missing.set(path, { path, by: new Set(), directory: isDirectory(path) });
    missing.get(path).by.add(by);
  }
  const all = [...missing.values()];
  const kept = all.filter((r) => !all.some((o) => o !== r && o.directory && r.path.startsWith(`${o.path}/`)));
  for (const r of kept) {
    for (const o of all) if (o !== r && r.directory && o.path.startsWith(`${r.path}/`)) for (const b of o.by) r.by.add(b);
  }
  return kept
    .sort((a, b) => a.path.localeCompare(b.path))
    .map((r) => ({ path: r.path, by: [...r.by].sort(), line: copyLineFor(r.path, r.directory) }));
}

/** The message a failing check prints. */
export function report(missing, insertAt) {
  const lines = ["The Dockerfile's frontend stage does not copy what the frontend tests and scripts read (lake#693):"];
  for (const m of missing) {
    lines.push(`  ${m.path} (read by ${m.by.join(", ")})`);
    lines.push(`    add to the frontend stage of Dockerfile, after line ${insertAt}:  ${m.line}`);
  }
  lines.push("The image builds frontend/ alone in /frontend; anything else it reads must be copied there.");
  return lines.join("\n");
}
