// SPDX-License-Identifier: MIT

// Preloaded into the frontend tests and scripts (`node --import ./scripts/watch-copied-reads.mjs`,
// lake#693). It records every file a process reads, lists, stats or imports in the repository
// outside frontend/, and at exit fails the process when the Dockerfile's `frontend` stage does not
// copy one of them, naming the path, who read it, and the COPY line to add.
//
// Watching the reads, rather than scanning the sources for "../", catches paths built in pieces
// (`join(import.meta.dirname, "..", "..", "assets")`) and reads inside helpers. `node --test` runs
// each test file in its own process with the same flags, so every file is watched and a file that
// reads an uncopied path fails on its own.
//
// In the image there is no Dockerfile beside frontend/ (the stage does not copy it), so the hook
// does nothing there: the COPY lines are then the truth, not the checkout.
import fs from "node:fs";
import module from "node:module";
import { dirname, isAbsolute, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { frontendCopies, report, uncopiedReads } from "./copied-paths.mjs";

const frontend = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repository = dirname(frontend);
const dockerfile = process.env.LAKE_FRONTEND_DOCKERFILE ?? resolve(repository, "Dockerfile");
const { existsSync, readFileSync, statSync } = fs;

if (existsSync(dockerfile)) {
  const reads = new Map();
  const by = relative(frontend, process.argv[1] ?? "node") || "node";

  const record = (target) => {
    let path;
    try {
      if (target instanceof URL) {
        if (target.protocol !== "file:") return;
        path = fileURLToPath(target);
      } else if (typeof target === "string") {
        path = target.startsWith("file:") ? fileURLToPath(target) : target;
      } else if (Buffer.isBuffer(target)) {
        path = target.toString();
      } else return;
    } catch {
      return;
    }
    const absolute = isAbsolute(path) ? path : resolve(path);
    const inRepository = relative(repository, absolute);
    if (!inRepository || inRepository.startsWith("..") || isAbsolute(inRepository)) return;
    if (inRepository === "frontend" || inRepository.startsWith(`frontend${sep}`)) return;
    if (absolute === dockerfile || inRepository === "Dockerfile") return; // the check's own input
    if (!existsSync(absolute)) return; // missing in the checkout too: the image behaves the same
    const key = inRepository.split(sep).join("/").replace(/\/+$/, "");
    if (!reads.has(key)) reads.set(key, { path: key, by });
  };

  const watch = (target, names) => {
    for (const name of names) {
      const original = target[name];
      if (typeof original !== "function") continue;
      target[name] = function (path, ...rest) {
        record(path);
        return original.call(this, path, ...rest);
      };
    }
  };
  watch(fs, [
    "readFileSync", "readFile", "readdirSync", "readdir", "existsSync", "exists", "statSync", "stat",
    "lstatSync", "lstat", "openSync", "open", "accessSync", "access", "createReadStream", "opendirSync",
    "opendir", "realpathSync", "realpath", "cpSync", "cp", "copyFileSync", "copyFile", "globSync", "glob",
  ]);
  watch(fs.promises, ["readFile", "readdir", "stat", "lstat", "open", "access", "opendir", "realpath", "cp", "copyFile", "glob"]);
  module.syncBuiltinESMExports();

  if (typeof module.registerHooks === "function") {
    module.registerHooks({
      resolve(specifier, context, next) {
        const resolved = next(specifier, context);
        if (resolved.url?.startsWith("file:")) record(resolved.url);
        return resolved;
      },
    });
  }

  process.on("exit", () => {
    if (reads.size === 0) return;
    const isDirectory = (path) => {
      try {
        return statSync(resolve(repository, path)).isDirectory();
      } catch {
        return false;
      }
    };
    const stage = frontendCopies(readFileSync(dockerfile, "utf8"));
    const missing = uncopiedReads([...reads.values()], stage.copies, isDirectory);
    if (missing.length === 0) return;
    process.stderr.write(`\n${report(missing, stage.insertAt)}\n`);
    process.exitCode = 1;
  });
}
