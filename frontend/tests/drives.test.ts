/** Drives (lake#561): paths, names, the upload and move plans, range selection, upload chunks and
 * what a shortcut opens; Drives → Storage and storage drives (lake#656). `npm test`; the build runs
 * it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { ancestors, chunkBySize, chunkRanges, cleanPath, datasetFormat, datasetName, driveSlug, expandTo, isWithin, joinPath, nameOf, parentOf, planMoves, planUpload, rangeSelect, sortEntries, validName } from "../src/pages/drives/paths.ts";
import { cleanTarget, nameAfterTarget, selectsOnFocus, storageLabel, targetHref, targetProblem } from "../src/pages/drives/targets.ts";
import { folderKey, matchLink, matchWhere, parentKey, type DriveMatch } from "../src/pages/drives/search.ts";
import { browsable, credentialFields, deleteWarning, driveForStore, endpointHint, phaseTone, storeFigures, storeKindWords, storeNeeds, storePhase, storeSizeText, storeWhere } from "../src/pages/drives/stores.ts";
import { STORAGE } from "../src/pages/drives/targets.ts";
import { movedFromDatasets } from "../src/pages/connections/connections.ts";
import { readFileSync } from "node:fs";

test("drive paths", () => {
  assert.equal(cleanPath("/a//b/./c/"), "a/b/c");
  assert.equal(joinPath("", "x"), "x");
  assert.equal(joinPath("a/b", "/c/"), "a/b/c");
  assert.equal(parentOf("a/b/c"), "a/b");
  assert.equal(parentOf("a"), "");
  assert.equal(nameOf("a/b/c.txt"), "c.txt");
  assert.deepEqual(ancestors("a/b"), ["", "a", "a/b"]);
  assert.ok(isWithin("a/b", "a"));
  assert.ok(!isWithin("ab", "a"));
  assert.deepEqual([...expandTo(new Set(["x"]), "a/b")].sort(), ["", "a", "a/b", "x"]);
});

test("a name lake accepts", () => {
  for (const good of ["Q3 report (final).pdf", "été", "a".repeat(255)]) assert.ok(validName(good), good);
  for (const bad of ["", ".", "..", ".lake-folder", "a/b", "a\\b", "tab\there", "é".repeat(128)]) assert.ok(!validName(bad), bad);
  assert.equal(driveSlug("Team Files — Q3!"), "team-files-q3");
});

test("folders first, then by the key", () => {
  const rows = [
    { path: "b.txt", kind: "file" as const, size: 5, modifiedAt: "2026-10-02" },
    { path: "z", kind: "folder" as const, size: 0 },
    { path: "a.txt", kind: "file" as const, size: 9, modifiedAt: "2026-10-01" },
    { path: "link", kind: "shortcut" as const, size: 0 },
  ];
  assert.deepEqual(sortEntries(rows, "name", "asc").map((r) => r.path), ["z", "a.txt", "b.txt", "link"]);
  assert.deepEqual(sortEntries(rows, "size", "desc").map((r) => r.path), ["z", "a.txt", "b.txt", "link"]);
});

test("shift-click selects a range", () => {
  const order = ["a", "b", "c", "d"];
  assert.deepEqual(rangeSelect(order, "b", "d"), ["b", "c", "d"]);
  assert.deepEqual(rangeSelect(order, "d", "a"), ["a", "b", "c", "d"]);
  assert.deepEqual(rangeSelect(order, null, "c"), ["c"]);
  assert.deepEqual(rangeSelect(order, "gone", "c"), ["c"]);
});

test("a folder dropped whole is uploaded with its folders first", () => {
  const plan = planUpload("Docs", [
    { relativePath: "Trip/Day 1/a.jpg", size: 3, file: "a" },
    { relativePath: "Trip/b.jpg", size: 2, file: "b" },
    { relativePath: "top.txt", size: 1, file: "t" },
    { relativePath: "bad/../x", size: 1, file: "x" },
  ]);
  assert.deepEqual(plan.folders, ["Docs/Trip", "Docs/Trip/Day 1"]);
  assert.deepEqual(plan.batches.map((b) => b.folder), ["Docs", "Docs/Trip", "Docs/Trip/Day 1"]);
  assert.deepEqual(plan.refused, ["bad/../x"]);
});

test("moving items onto a folder", () => {
  const { moves, skipped } = planMoves(["a/x", "a/x/y", "b", "c/z"], "c");
  assert.deepEqual(moves, [
    { from: "a/x", to: "c/x" },
    { from: "b", to: "c/b" },
  ]);
  assert.deepEqual(skipped.map((s) => s.path), ["c/z"]);
  assert.equal(planMoves(["c"], "c/d").skipped[0].reason, "a folder cannot move into itself");
});

test("uploads in requests that fit, and large files in whole blocks", () => {
  const { chunks, tooLarge } = chunkBySize(
    [
      { name: "a", size: 30 },
      { name: "b", size: 30 },
      { name: "huge", size: 1000 },
    ],
    400,
  );
  assert.deepEqual(chunks.map((c) => c.map((f) => f.name)), [["a"], ["b"]]);
  assert.deepEqual(tooLarge.map((f) => f.name), ["huge"]);
  const MiB = 1024 * 1024;
  assert.deepEqual(chunkRanges(20 * MiB + 5, MiB, 8 * MiB), [
    { offset: 0, length: 8 * MiB },
    { offset: 8 * MiB, length: 8 * MiB },
    { offset: 16 * MiB, length: 4 * MiB + 5 },
  ]);
  assert.deepEqual(chunkRanges(0, MiB, 8 * MiB), []);
  assert.deepEqual(chunkRanges(10, MiB, 100), [{ offset: 0, length: 10 }], "never less than one block a chunk");
});

test("what a shortcut opens", () => {
  assert.equal(targetHref({ kind: "repository", repository_id: "r1" })?.href, "/repository?id=r1");
  assert.equal(targetHref({ kind: "repository_path", repository_id: "r1", path: "docs/q3.pdf" })?.href, "/drive/preview?repository_id=r1&ref=main&path=docs%2Fq3.pdf");
  assert.equal(targetHref({ kind: "repository_path", repository_id: "r1", path: "docs" })?.href, "/drive?repository_id=r1&ref=main&path=docs");
  assert.deepEqual(targetHref({ kind: "drive_entry", drive_id: "d", entry_id: "e" }), { href: "/drives/drive?id=d&focus=e", external: false, workspace: true });
  assert.equal(targetHref({ kind: "dataset", id: "sales/orders" })?.href, "/train/dataset?name=orders&database=sales");
  assert.equal(targetHref({ kind: "transcript", id: "t" })?.href, "/transcript?id=t");
  assert.equal(targetHref({ kind: "project", name: "churn" })?.href, "/design/project?name=churn");
  assert.equal(targetHref({ kind: "model", id: "m 1" })?.href, "/models/m%201");
  assert.deepEqual(targetHref({ kind: "url", url: "https://x.test/" }), { href: "https://x.test/", external: true, workspace: false });
  assert.equal(targetHref({ kind: "url", url: "javascript:alert(1)" }), null);
  assert.equal(storageLabel("files"), "Workspace files");
});

test("a shortcut's target is checked as lake checks it", () => {
  assert.equal(targetProblem({ kind: "repository" }), "Choose a repository.");
  assert.equal(targetProblem({ kind: "repository_path", repository_id: "r" }), "Name the path in the repository.");
  assert.equal(targetProblem({ kind: "url", url: "ftp://x" }), "A web address starts with http:// or https://.");
  assert.equal(targetProblem({ kind: "url", url: "https://x.test" }), null);
  assert.deepEqual(cleanTarget({ kind: "repository_path", repository_id: " r ", path: "/a/b/", id: "stray", url: "x" }), { kind: "repository_path", repository_id: "r", path: "a/b" });
});

test("Load as dataset is offered for CSV and JSONL files, under a name SQL can query (lake#593)", () => {
  assert.equal(datasetFormat("churn.csv"), "csv");
  assert.equal(datasetFormat("People.JSONL"), "jsonl");
  assert.equal(datasetFormat("events.ndjson"), "jsonl");
  assert.equal(datasetFormat("notes.md"), null);
  assert.equal(datasetFormat("csv"), null);
  assert.equal(datasetName("Churn 2026.csv"), "churn_2026");
  assert.equal(datasetName("2026-q3.csv"), "_2026_q3");
  assert.equal(datasetName("--.csv"), "dataset");
  assert.equal(datasetName("a".repeat(80) + ".csv").length, 63);
  assert.match(datasetName("Ünïcode name.jsonl"), /^[A-Za-z_][A-Za-z0-9_]{0,62}$/);
});

test("New shortcut: a suggested name is replaced by typing, a typed one is kept (lake#580)", () => {
  assert.equal(nameAfterTarget("", false, "app-e2e-notes"), "app-e2e-notes");
  assert.equal(nameAfterTarget("app-e2e-notes", false, "other"), "other", "a new target suggests again");
  assert.equal(nameAfterTarget("notes app source", true, "app-e2e-notes"), "notes app source", "what was typed stays");
  assert.equal(selectsOnFocus("app-e2e-notes", false), true, "focus selects the suggestion, so typing replaces it");
  assert.equal(selectsOnFocus("notes app source", true), false);
  assert.equal(selectsOnFocus("", false), false);
});

test("where a drive search match opens", () => {
  const scope = { customerId: "c", workspaceId: "w" };
  const files = { id: "d1", name: "Team", storage: "files" as const, repository_id: null };
  const repo = { id: "d2", name: "Archive", storage: "repository" as const, repository_id: "r1" };
  const match = (m: Partial<DriveMatch>): DriveMatch => ({ drive: files, kind: "file", id: null, parent_id: null, name: "", path: "", folder: "", ...m });

  assert.deepEqual(matchLink(match({ kind: "drive", name: "Team" }), scope), { to: "/drives/drive?id=d1", workspace: true });
  assert.equal(matchWhere(match({ kind: "drive" })), "Workspace files drive");

  const folder = match({ kind: "folder", id: "f1", parent_id: "f0", name: "Day 1", path: "Trip/Day 1", folder: "Trip" });
  assert.deepEqual(matchLink(folder, scope), { to: "/drives/drive?id=d1&folder=f1", workspace: true });
  assert.equal(folderKey(folder), "f1");
  assert.equal(parentKey(folder), "f0");
  assert.equal(matchWhere(folder), "Team / Trip");

  // A repository drive addresses its folders by path.
  const repoFolder = match({ drive: repo, kind: "folder", name: "Reports", path: "Reports", folder: "" });
  assert.deepEqual(matchLink(repoFolder, scope), { to: "/drives/drive?id=d2&folder=Reports", workspace: true });
  assert.equal(parentKey(repoFolder), "");
  assert.equal(matchWhere(repoFolder), "Archive");
  assert.deepEqual(matchLink(match({ drive: repo, kind: "file", path: "Reports/q3.csv", folder: "Reports" }), scope), {
    to: "/drive/preview?repository_id=r1&ref=main&path=Reports%2Fq3.csv",
    workspace: false,
  });

  // A workspace file opens in the browser when it can show it, and downloads otherwise.
  assert.deepEqual(matchLink(match({ id: "e1", content_type: "application/pdf" }), scope), {
    href: "/drive/files/download?customer_id=c&workspace_id=w&drive_id=d1&id=e1&inline=1",
    newTab: true,
    download: false,
  });
  assert.deepEqual(matchLink(match({ id: "e2", content_type: "application/zip" }), scope), {
    href: "/drive/files/download?customer_id=c&workspace_id=w&drive_id=d1&id=e2",
    newTab: false,
    download: true,
  });

  // A shortcut opens its target; a broken one shows where it is.
  const shortcut = match({ kind: "shortcut", id: "s1", target: { kind: "url", url: "https://orama.test/" }, status: "unknown" });
  assert.deepEqual(matchLink(shortcut, scope), { href: "https://orama.test/", newTab: true, download: false });
  assert.deepEqual(matchLink({ ...shortcut, target: { kind: "transcript", id: "t1" } }, scope), { to: "/transcript?id=t1", workspace: true });
  assert.deepEqual(matchLink({ ...shortcut, status: "broken", target: { kind: "transcript", id: "t1" } }, scope), { to: "/drives/drive?id=d1&focus=s1", workspace: true });
});

test("Drives → Storage: each store's state, figures and whether it opens as a drive (lake#656)", () => {
  const providers = [{ type: "volume", provider: "cluster", label: "Volume — cluster disk" }];
  assert.deepEqual(storeKindWords(providers, "volume", "cluster"), ["Volume", "cluster disk"]);
  assert.deepEqual(storeKindWords(providers, "object", "r2"), ["object", "r2"]);
  assert.equal(storePhase({ phase: "Ready", connected: true }), "Ready");
  assert.equal(storePhase({ phase: "Ready", connected: false }), "Connecting");
  assert.equal(storePhase({}), "Waiting for the operator");
  assert.equal(storePhase({ desired: "absent", phase: "Ready" }), "Deleting");
  assert.equal(phaseTone("Failed"), "danger");
  // A volume is shown, not browsed; a bucket or filesystem opens once Ready.
  assert.equal(browsable({ type: "volume", phase: "Ready" }), "volume");
  assert.equal(browsable({ type: "object", phase: "Ready" }), "yes");
  assert.equal(browsable({ type: "filesystem", phase: "Ready", connected: false }), "not_ready");
  assert.equal(browsable({ type: "network_filesystem", phase: "Ready" }), "other");
  const stores = [
    { id: "a", type: "volume", phase: "Ready", size_gi: 10 },
    { id: "b", type: "object", phase: "Ready", size_gi: 5 },
    { id: "c", type: "object", phase: "Provisioning", size_gi: 1 },
    { id: "d", type: "volume", desired: "absent", phase: "Ready", size_gi: 50 },
  ];
  assert.deepEqual(storeFigures(stores), { count: 4, changing: 2, browsable: 1, volumes: 1, sizeGi: 16 });
  assert.deepEqual(storeFigures([]), { count: 0, changing: 0, browsable: 0, volumes: 0, sizeGi: 0 });
  assert.equal(driveForStore([{ id: "d1", store_id: "b" }, { id: "d2", store_id: null }], "b")?.id, "d1");
  assert.equal(driveForStore([{ id: "d2", store_id: null }], "b"), undefined);
});

test("a storage drive is a fourth kind, and an old #storage link opens Drives → Storage", () => {
  const store = STORAGE.find((s) => s.id === "store");
  assert.ok(store);
  assert.equal(store.label, "Storage");
  assert.match(store.detail, /never deletes the storage/);
  assert.equal(storageLabel("store"), "Storage");
  assert.equal(movedFromDatasets("", "#storage"), "/drives?tab=storage");
  // A match in a storage drive opens by its path, as an entry id.
  const match: DriveMatch = {
    drive: { id: "d", name: "Media", storage: "store", repository_id: null, store_id: "s" },
    kind: "folder",
    id: "Reports/2026",
    parent_id: "Reports",
    name: "2026",
    path: "Reports/2026",
    folder: "Reports",
  };
  assert.equal(folderKey(match), "Reports/2026");
  assert.equal(parentKey(match), "Reports");
  assert.deepEqual(matchLink(match, { customerId: "c", workspaceId: "w" }), { to: "/drives/drive?id=d&folder=Reports%2F2026", workspace: true });
});

test("Datasets keeps datasets only: no storage and no connections on its page (lake#651, lake#656)", () => {
  const page = readFileSync(new URL("../src/pages/train/DataPage.tsx", import.meta.url), "utf8");
  for (const gone of ["list_stores", "create_store", "delete_store", "store_connection", "list_store_providers", "connect_service", "delete_connection", "list_source_tables"]) {
    assert.ok(!page.includes(`"${gone}"`), `Datasets no longer calls ${gone}`);
  }
});

test("New storage asks each provider for what it needs, and deleting says what stays (lake#664)", () => {
  assert.equal(storeNeeds("volume", "cluster"), "size");
  for (const provider of ["r2", "s3", "b2", "gcs", "azure"]) assert.equal(storeNeeds("object", provider), "bucket", provider);
  assert.equal(storeNeeds("object", "lake"), "repository");
  assert.equal(storeNeeds("filesystem", "lake"), "repository");
  assert.equal(storeNeeds("filesystem", "zetta"), "none");
  assert.equal(endpointHint("r2").required, true);
  assert.equal(endpointHint("azure").required, true);
  assert.equal(endpointHint("s3").required, false);
  assert.match(credentialFields("azure"), /account_key/);
  assert.match(credentialFields("s3"), /access_key_id/);
  assert.equal(deleteWarning({ type: "object", provider: "r2" }).tone, "quiet");
  assert.match(deleteWarning({ type: "object", provider: "r2" }).text, /stay in your account/);
  assert.match(deleteWarning({ type: "filesystem", provider: "lake" }).text, /repository stays/);
  assert.equal(deleteWarning({ type: "volume", provider: "cluster" }).tone, "warn");
  assert.equal(deleteWarning({ type: "object", provider: "zetta" }).tone, "warn");
  assert.equal(storeSizeText({ type: "volume", size_gi: 10 }), "10 GiB");
  assert.equal(storeSizeText({ type: "object", size_gi: 0 }), "—");
  assert.equal(storeWhere({ provider: "r2", location: { bucket: "media", prefix: "team" } }), "media/team");
  assert.equal(storeWhere({ provider: "zetta", location: null }), "");
  assert.equal(storeWhere({ provider: "cluster" }), "");
});
