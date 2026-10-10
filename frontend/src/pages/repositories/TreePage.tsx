/** /repository/tree?id=&ref= (web_ui_repository_tree): every path of a ref with the newest change
 * that touched it. A directory "opens" by an anchor jump: the tree is recursive. */
import { useEffect } from "react";
import { Link, useLocation, useSearchParams } from "react-router-dom";
import { dateTime } from "@/lib/format";
import {
  ForgePage,
  ForgeTabs,
  Identifier,
  itemsOf,
  pending,
  PathBreadcrumbs,
  repositoryName,
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
  return (
    <ForgePage
      above={<PathBreadcrumbs repository={r} refName={refName} path="" />}
      eyebrow="repository tree"
      title={repositoryName(r)}
      sub={
        <>
          Browse <code>{refName}</code> by path and last change.
        </>
      }
    >
      <ForgeTabs repository={r} current="code" />
      {entries.length === 0 ? (
        <div className="panel empty">
          <strong>This ref has no files.</strong>
          <p>Push a change to publish its first tree.</p>
        </div>
      ) : (
        <section className="panel tree-browser">
          <div className="tree-browser-heading">
            <span>Path</span>
            <span>Last change</span>
          </div>
          <ul className="tree-view">
            {entries.map((entry) => {
              const change = last.get(entry.path);
              return (
                <li key={entry.path} id={entry.path}>
                  <div className="tree-entry-name">
                    <span className="tree-kind" aria-hidden="true">
                      {entry.kind === "blob" ? "·" : "›"}
                    </span>
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
                        {change.message} · {dateTime(change.occurred_at)}
                      </span>
                    )}
                  </div>
                </li>
              );
            })}
          </ul>
        </section>
      )}
    </ForgePage>
  );
}
