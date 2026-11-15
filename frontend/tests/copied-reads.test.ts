// SPDX-License-Identifier: MIT

// The check that the image's frontend stage copies what the tests and scripts read (lake#693).
// Negative cases reproduce the two image builds that failed on 2026-10-11: lake#639 read
// assets/catalogue-icons/ and lake#653 read src/api/engine_openapi.json from beside frontend/.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { copyLineFor, frontendCopies, report, uncopiedReads } from "../scripts/copied-paths.mjs";

const COPIED = `FROM node:22-bookworm-slim AS frontend
WORKDIR /frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY frontend/ ./
COPY assets/catalogue-icons/ /assets/catalogue-icons/
COPY src/api/engine_openapi.json /src/api/engine_openapi.json
RUN npm run build

FROM ubuntu:24.04 AS build
COPY . /src
`;
// The same stage before lake#678 and lake#692 added the two lines.
const UNCOPIED = COPIED.replace(/^COPY (assets|src\/api).*\n/gm, "");

const directories = new Set(["assets", "assets/catalogue-icons", "src", "src/api"]);
const isDirectory = (path: string) => directories.has(path);
const READS = [
  { path: "assets/catalogue-icons", by: "tests/catalogue-icons.test.ts" },
  { path: "assets/catalogue-icons/solutions/a.svg", by: "tests/catalogue-icons.test.ts" },
  { path: "src/api/engine_openapi.json", by: "tests/agents.test.ts" },
];

test("frontendCopies reads only the frontend stage, from its working directory", () => {
  const stage = frontendCopies(COPIED);
  assert.equal(stage.found, true);
  assert.equal(stage.insertAt, 7);
  assert.deepEqual(
    stage.copies.map((c: { source: string; dest: string }) => [c.source, c.dest]),
    [
      ["frontend/package.json", "/frontend"],
      ["frontend/package-lock.json", "/frontend"],
      ["frontend", "/frontend"],
      ["assets/catalogue-icons", "/assets/catalogue-icons"],
      ["src/api/engine_openapi.json", "/src/api/engine_openapi.json"],
    ],
  );
});

test("reads the stage copies pass", () => {
  assert.deepEqual(uncopiedReads(READS, frontendCopies(COPIED).copies, isDirectory), []);
});

test("an uncopied folder and file are named with the COPY line to add (lake#639, lake#653)", () => {
  const stage = frontendCopies(UNCOPIED);
  const missing = uncopiedReads(READS, stage.copies, isDirectory);
  assert.deepEqual(missing, [
    { path: "assets/catalogue-icons", by: ["tests/catalogue-icons.test.ts"], line: "COPY assets/catalogue-icons/ /assets/catalogue-icons/" },
    { path: "src/api/engine_openapi.json", by: ["tests/agents.test.ts"], line: "COPY src/api/engine_openapi.json /src/api/engine_openapi.json" },
  ]);
  const text = report(missing, stage.insertAt);
  assert.match(text, /after line 5: {2}COPY assets\/catalogue-icons\/ \/assets\/catalogue-icons\//);
});

test("a copy to a different place does not count: the test reads /<path>", () => {
  const moved = UNCOPIED.replace("RUN npm run build", "COPY src/api/engine_openapi.json /frontend/engine_openapi.json\nRUN npm run build");
  const missing = uncopiedReads([READS[2]], frontendCopies(moved).copies, isDirectory);
  assert.equal(missing[0]?.line, copyLineFor("src/api/engine_openapi.json", false));
});

// End to end through the preload, with the checkout's real files. Skipped in the image, where the
// Dockerfile is not beside frontend/ and the hook stands down.
const repository = join(import.meta.dirname, "..", "..");
const inCheckout = existsSync(join(repository, "Dockerfile"));

test("the preload fails a process that reads what the stage does not copy", { skip: !inCheckout }, () => {
  const dir = mkdtempSync(join(tmpdir(), "lake-693-"));
  try {
    const dockerfile = join(dir, "Dockerfile");
    const script = join(dir, "reads.mjs");
    writeFileSync(
      script,
      `import { readdirSync, readFileSync } from "node:fs";
       readdirSync(${JSON.stringify(join(repository, "assets", "catalogue-icons"))});
       JSON.parse(readFileSync(${JSON.stringify(join(repository, "src", "api", "engine_openapi.json"))}, "utf8"));`,
    );
    const hook = join(import.meta.dirname, "..", "scripts", "watch-copied-reads.mjs");
    const run = (stage: string) => {
      writeFileSync(dockerfile, stage);
      return spawnSync(process.execPath, ["--import", hook, script], { env: { ...process.env, LAKE_FRONTEND_DOCKERFILE: dockerfile }, encoding: "utf8" });
    };

    const failed = run(UNCOPIED);
    assert.equal(failed.status, 1, failed.stderr);
    assert.match(failed.stderr, /assets\/catalogue-icons \(read by /);
    assert.match(failed.stderr, /COPY assets\/catalogue-icons\/ \/assets\/catalogue-icons\//);
    assert.match(failed.stderr, /COPY src\/api\/engine_openapi\.json \/src\/api\/engine_openapi\.json/);

    const passed = run(COPIED);
    assert.equal(passed.status, 0, passed.stderr);
    assert.equal(passed.stderr, "");
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
