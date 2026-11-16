// SPDX-License-Identifier: MIT

/** /repository/tree?id=&ref= (web_ui_repository_tree): every path of a ref with the newest change
 * that touched it, filtered by path in the page, beside the ref's figures. A directory "opens" by an
 * anchor jump: the tree is recursive. */
import { useEffect, useState } from "react";
import { FolderTree } from "lucide-react";
import { Card, Empty, KeyValue, SearchInput, Split, Toolbar } from "@/lib/ui";
import { Link, useLocation, useSearchParams } from "react-router-dom";
import { dateTime, relative } from "@/lib/format";
import {
  EntryIcon,
  ForgePage,
  Identifier,
  itemsOf,
  pending,
  PathBreadcrumbs,
  ToRepositories,
  useChangeDiffs,
  useForgeRead,
  useRepositoryTrail,
  byteCount,
  type Change,
  type Items,
  type Repository,
  type TreeEntry,
} from "./forge";

export default function TreePage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const ref = params.get("ref") ?? "";
  if (!id || !ref) return <ToRepositories />;
  return <Tree id={id} refName={ref} />;
}

/** The newest change touching each path; a directory also counts changes below it. */
function lastChanges(entries: TreeEntry[], changes: Change[], diffs: { path: string }[][]): Map<string, Change> {
  const last = new Map<string, Change>();
  changes.forEach((change, index) => {
    for (const difference of diffs[index] ?? []) {
      for (const entry of entries) {
        if (last.has(entry.path)) continue;
        const inside = entry.kind !== "blob" && difference.path.startsWith(`${entry.path}/`);
        if (difference.path === entry.path || inside) last.set(entry.path, change);
      }
    }
  });
  return last;
}

function Tree({ id, refName }: { id: string; refName: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const tree = useForgeRead<Items<TreeEntry>>("repository_tree", { id, ref: refName });
  const changes = useForgeRead<Items<Change>>("list_changes", { id });
  const diffs = useChangeDiffs(id, changes.data ? itemsOf(changes.data) : undefined);
  const location = useLocation();
  const [filter, setFilter] = useState("");
  useRepositoryTrail(repository.data, "Code");
  const ready = !!diffs.data && !!tree.data;
  useEffect(() => {
    if (!ready || !location.hash) return;
    document.getElementById(decodeURIComponent(location.hash.slice(1)))?.scrollIntoView();
  }, [ready, location.hash]);
  const wait = pending(repository, tree, changes, diffs);
  if (wait) return wait;
  const r = repository.data!;
  const entries = itemsOf(tree.data);
  const last = lastChanges(entries, itemsOf(changes.data), diffs.data!);
  const encoded = encodeURIComponent(id);
  const blobs = entries.filter((entry) => entry.kind === "blob");
  const total = blobs.reduce((sum, entry) => sum + entry.size, 0);
  const needle = filter.trim().toLowerCase();
  const shown = needle ? entries.filter((entry) => entry.path.toLowerCase().includes(needle)) : entries;
  return (
    <ForgePage
      above={<PathBreadcrumbs repository={r} refName={refName} path="" />}
      title="Files"
      repository={r}
      tab="code"
      sub={
        <>
          Browse <code>{refName}</code> by path and last change.
        </>
      }
    >
      {entries.length === 0 ? (
        <Empty icon={FolderTree} title="This ref has no files">
          Push a change to publish its first tree.
        </Empty>
      ) : (
        <Split
          side={
            <Card label="This ref">
              <KeyValue
                rows={[
                  ["Ref", <code>{refName}</code>],
                  ["Files", <span className="font-mono">{blobs.length}</span>],
                  ["Directories", <span className="font-mono">{entries.length - blobs.length}</span>],
                  ["Size", byteCount(total)],
                  ["Changes", <Link to={`/changes?repository_id=${encoded}`}>{itemsOf(changes.data).length} in the log</Link>],
                ]}
              />
            </Card>
          }
        >
          <Toolbar>
            <SearchInput value={filter} onChange={setFilter} placeholder="Filter paths…" label="Filter paths" />
            <span className="ml-auto font-mono text-[12px] text-ink-3">
              {shown.length} of {entries.length} paths
            </span>
          </Toolbar>
          <section className="panel tree-browser">
            <div className="tree-browser-heading">
              <span>Path</span>
              <span>Last change</span>
            </div>
            <ul className="tree-view">
              {shown.map((entry) => {
                const change = last.get(entry.path);
                return (
                  <li key={entry.path} id={entry.path}>
                    <div className="tree-entry-name">
                      <EntryIcon kind={entry.kind === "blob" ? "blob" : "tree"} size={14} />
                      <div>
                        {entry.kind === "blob" ? (
                          <Link to={`/file?repository_id=${encoded}&ref=${encodeURIComponent(refName)}&path=${encodeURIComponent(entry.path)}`}>{entry.path}</Link>
                        ) : (
                          <a href={`#${encodeURIComponent(entry.path)}`}>{entry.path}</a>
                        )}
                        <span className="tree-entry-meta">
                          {entry.kind} · <code>{entry.mode.toString(8)}</code>
                          {entry.kind === "blob" && ` · ${byteCount(entry.size)}`}
                        </span>
                      </div>
                    </div>
                    <div className="tree-last-change">
                      {change ? <Identifier value={change.id} /> : <span className="muted">No recorded change</span>}
                      {change?.message && (
                        <span className="muted">
                          {change.message} · <span title={dateTime(change.occurred_at)}>{relative(change.occurred_at)}</span>
                        </span>
                      )}
                    </div>
                  </li>
                );
              })}
              {shown.length === 0 && <li className="muted">No path matches “{filter}”.</li>}
            </ul>
          </section>
        </Split>
      )}
    </ForgePage>
  );
}
