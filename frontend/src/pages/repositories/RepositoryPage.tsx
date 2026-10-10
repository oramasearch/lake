/** /repository?id= (web_ui_repository): the repository home. Files, recent changes and the README
 * on the left; clone and push, references and releases, languages, effective access and About on
 * the right. Public repositories render for visitors. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ApiError } from "@/lib/api";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Badge, Button } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { Markdown } from "./Markdown";
import {
  byteCount,
  fileLanguage,
  ForgeBadge,
  ForgeTabs,
  forgeAct,
  Identifier,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  type AccessEntry,
  type Change,
  type ForgeRecord,
  type Items,
  type Ref,
  type Repository,
  type TreeEntry,
} from "./forge";

type Customer = { id: string; name: string; user_id: string | null };
type Workspace = { id: string; name: string; customer_id: string };
type RepositoryFile = { path: string; contents: string };

/** `shell_argument`: single quotes, a quote written as '\''. */
function shellArgument(value: string): string {
  return `'${value.replace(/'/g, "'\\''")}'`;
}

export default function RepositoryPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  if (!id) return <ToRepositories />;
  return <RepositoryHome id={id} />;
}

function RepositoryHome({ id }: { id: string }) {
  const { viewer } = useSession();
  const signedIn = viewer.signed_in;
  const repository = useForgeRead<Repository>("get_repository", { id });
  const customerId = repository.data?.customer_id;
  const owner = useForgeRead<Customer>(signedIn && customerId ? "get_customer" : null, { customer_id: customerId }, { retry: false });
  const refs = useForgeRead<Items<Ref>>("repository_refs", { id });
  const changes = useForgeRead<Items<Change>>("list_changes", { id });
  const refList = itemsOf(refs.data);
  const defaultRef = refList[0]?.name ?? "main";
  const hasRefs = refs.data !== undefined && refList.length > 0;
  const tree = useForgeRead<Items<TreeEntry>>(hasRefs ? "repository_tree" : null, { id, ref: defaultRef });
  const readme = useForgeRead<RepositoryFile>(hasRefs ? "repository_file" : null, { id, ref: defaultRef, path: "README.md" }, { retry: false });
  const access = useForgeRead<Items<AccessEntry>>(signedIn ? "entity_effective_access" : null, { object_type: "repository", id }, { retry: false });
  const releases = useForgeRead<Items<ForgeRecord>>("list_releases", { repository_id: id }, { retry: false });
  const workspaceId = repository.data?.workspace_id;
  const workspace = useForgeRead<Workspace>(signedIn && customerId && workspaceId ? "get_workspace" : null, { customer_id: customerId, workspace_id: workspaceId }, { retry: false });

  const ownerName = owner.data?.name ?? repository.data?.customer_id ?? "";
  const title = repository.data ? `${ownerName}/${repository.data.name}` : "";
  useTrail(repository.data ? [{ label: "Repositories", to: "/repositories" }, entityCrumb(title, `/repository?id=${encodeURIComponent(id)}`)] : null);

  // A README that is not there is expected; any other failure fails the page, as on the server.
  const readmeFailed = readme.isError && !(readme.error instanceof ApiError && readme.error.code === "not_found");
  const wait = pending(repository, refs, changes, ...(hasRefs ? [tree] : []), ...(readmeFailed ? [readme] : []));
  if (wait) return wait;
  const r = repository.data!;
  const entries = hasRefs ? itemsOf(tree.data) : [];
  const recent = itemsOf(changes.data);
  const blobs = entries.filter((entry) => entry.kind === "blob");
  const languages = new Map<string, number>();
  for (const entry of blobs) languages.set(fileLanguage(entry.path), (languages.get(fileLanguage(entry.path)) ?? 0) + entry.size);
  const total = blobs.reduce((sum, entry) => sum + entry.size, 0);
  const encoded = encodeURIComponent(id);
  const refQuery = encodeURIComponent(defaultRef);
  const origin = viewer.external_origin ?? "";
  const url = shellArgument(`${origin}/v1/repositories/${id}`);
  const directory = shellArgument(`./${r.name}`);

  return (
    <div className="page-container">
      <div className="forge-heading">
        <div>
          <p className="eyebrow">repository</p>
          <h1>{title}</h1>
          <p className="sub">{r.description_unavailable ? `Description unavailable: ${r.description_unavailable}` : r.description || "No description."}</p>
        </div>
        <Badge>{r.visibility}</Badge>
      </div>
      <ForgeTabs repository={r} current="code" />
      <div className="repository-layout">
        <div className="repository-main">
          {workspace.data ? (
            <p className="muted repository-workspace">
              Workspace <Link to={`/w/${encodeURIComponent(workspace.data.id)}/design/projects`}>{workspace.data.name}</Link>
            </p>
          ) : signedIn && !r.workspace_id ? (
            <div className="notice">
              <strong>No workspace yet.</strong> This repository was created before workspaces and has not been assigned one.
            </div>
          ) : null}
          {r.deleted_at && (
            <div className="notice status-danger">
              <strong>Tombstoned.</strong> Deleted {dateTime(r.deleted_at)}. Collection may reclaim unreferenced content after {dateTime(r.retention_until)}.
            </div>
          )}
          {signedIn && !r.deleted_at && <BuildImage repository={r} revision={defaultRef} />}
          <section className="panel repository-browser">
            <div className="section-heading">
              <div>
                <h2>Files</h2>
                <p className="muted">
                  The published tree at <code>{defaultRef}</code>.
                </p>
              </div>
              <Link to={`/repository/tree?id=${encoded}&ref=${refQuery}`}>Browse tree</Link>
            </div>
            {entries.length === 0 ? (
              <div className="empty compact-empty">No files are published on this ref.</div>
            ) : (
              <details className="tree-group" open>
                <summary>Repository root</summary>
                <ul className="forge-tree">
                  {entries.slice(0, 12).map((entry) => (
                    <li key={entry.path}>
                      <span className="tree-kind" aria-hidden="true">
                        {entry.kind === "blob" ? "·" : "›"}
                      </span>
                      <Link
                        to={
                          entry.kind === "blob"
                            ? `/file?repository_id=${encoded}&ref=${refQuery}&path=${encodeURIComponent(entry.path)}`
                            : `/repository/tree?id=${encoded}&ref=${refQuery}#${encodeURIComponent(entry.path)}`
                        }
                      >
                        {entry.path}
                      </Link>
                      <span className="tree-size">{entry.kind === "blob" ? byteCount(entry.size) : "directory"}</span>
                    </li>
                  ))}
                </ul>
              </details>
            )}
          </section>
          <section className="panel recent-changes">
            <div className="section-heading">
              <h2>Recent Changes</h2>
              <Link to={`/changes?repository_id=${encoded}`}>All changes</Link>
            </div>
            {recent.length === 0 ? (
              <div className="empty compact-empty">No changes have been cut.</div>
            ) : (
              <ol className="change-list">
                {recent.slice(0, 5).map((change) => (
                  <li key={change.id}>
                    <div>
                      <strong>
                        <Link to={`/change?repository_id=${encoded}&id=${encodeURIComponent(change.id)}`}>{change.message || "Untitled change"}</Link>
                      </strong>
                      <span className="muted">
                        {change.author} · {dateTime(change.occurred_at)}
                      </span>
                    </div>
                    <Identifier value={change.id} />
                  </li>
                ))}
              </ol>
            )}
          </section>
          <section className="panel readme-panel">
            <div className="section-heading">
              <h2>README</h2>
            </div>
            {readme.data ? <Markdown>{readme.data.contents}</Markdown> : <div className="empty compact-empty">Add a README.md to explain this repository.</div>}
          </section>
        </div>
        <aside className="repository-sidebar" aria-label="Repository details">
          <section className="panel">
            <h2>Clone and Push</h2>
            {!origin ? (
              <p className="muted">
                An administrator must set <code>LAKE_EXTERNAL_ORIGIN</code> or <code>LAKE_PUBLIC_HOST</code> before clone and push commands are available.
              </p>
            ) : (
              <>
                <p className="muted">
                  Create an account token on this instance under <Link to="/tokens">Tokens</Link> and export it as <code>LIQUID_TOKEN</code> in your terminal. Use a token issued by this
                  instance only.
                </p>
                <p>First checkout into a new directory:</p>
                <pre>
                  <code>{`liquid clone ${url} ${directory}\ncd ${directory}`}</code>
                </pre>
                <p>From that checkout, after capturing and naming your changes:</p>
                <pre>
                  <code>{`liquid push ${url}`}</code>
                </pre>
              </>
            )}
          </section>
          <section className="panel">
            <h2>References</h2>
            {refList.length === 0 ? (
              <p className="muted">No references are published.</p>
            ) : (
              <ul className="detail-list">
                {refList.map((ref, index) => (
                  <li key={ref.name}>
                    <div>
                      <span>
                        <Link to={`/repository/tree?id=${encoded}&ref=${encodeURIComponent(ref.name)}`}>{ref.name}</Link>
                        {index === 0 && (
                          <>
                            {" "}
                            <Badge>default</Badge>
                          </>
                        )}
                      </span>
                    </div>
                    <Identifier value={ref.change} />
                  </li>
                ))}
              </ul>
            )}
            {releases.isError ? (
              <>
                <h3>Releases</h3>
                <p className="muted">Releases are unavailable: {(releases.error as Error).message}</p>
              </>
            ) : itemsOf(releases.data).length > 0 ? (
              <>
                <h3>Releases</h3>
                <ul className="detail-list">
                  {itemsOf(releases.data).map((release) => (
                    <li key={release.id}>
                      <strong>{release.value.tag ?? "untagged"}</strong>
                      <Identifier value={release.value.change_id ?? ""} />
                    </li>
                  ))}
                </ul>
              </>
            ) : null}
          </section>
          <section className="panel">
            <h2>Languages</h2>
            {languages.size === 0 ? (
              <p className="muted">No indexed file sizes yet.</p>
            ) : (
              <>
                <ul className="language-summary">
                  {[...languages.entries()]
                    .sort(([a], [b]) => (a < b ? -1 : 1))
                    .map(([language, bytes]) => (
                      <li key={language}>
                        <span>{language}</span>
                        <strong>{byteCount(bytes)}</strong>
                      </li>
                    ))}
                </ul>
                <p className="muted">
                  {byteCount(total)} indexed across {blobs.length} files.
                </p>
              </>
            )}
          </section>
          <section className="panel">
            <h2>Effective Access</h2>
            {access.isError ? (
              <p className="muted">Effective access is unavailable: {(access.error as Error).message}</p>
            ) : itemsOf(access.data).length === 0 ? (
              <p className="muted">No direct or inherited grants are visible.</p>
            ) : (
              <ul className="detail-list">
                {itemsOf(access.data).map((entry, index) => (
                  <li key={index}>
                    <Identifier value={entry.principal_taap_id} />
                    <ForgeBadge value={entry.role} />
                  </li>
                ))}
              </ul>
            )}
            <p>{signedIn && <Link to={`/repository/access?id=${encoded}`}>View access details</Link>}</p>
          </section>
          <section className="panel repository-meta">
            <h2>About</h2>
            <dl>
              <dt>Identifier</dt>
              <dd>
                <Identifier value={r.id} />
              </dd>
              <dt>Owner</dt>
              <dd>
                <Link to={`/repositories?customer_id=${encodeURIComponent(r.customer_id)}`}>{ownerName}</Link>
              </dd>
              <dt>Created</dt>
              <dd>{dateTime(r.created_at)}</dd>
              <dt>Updated</dt>
              <dd>{dateTime(r.updated_at)}</dd>
            </dl>
          </section>
        </aside>
      </div>
    </div>
  );
}

/** "Build image": queue an in-cluster build of the default ref as `<name>:latest`; success opens
 * the workspace's Packages page, where the build shows (serve_repository_build). */
function BuildImage({ repository, revision }: { repository: Repository; revision: string }) {
  const navigate = useNavigate();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const build = async () => {
    setBusy(true);
    setError(null);
    try {
      await forgeAct("build_image", {
        customer_id: repository.customer_id,
        ...(repository.workspace_id ? { workspace_id: repository.workspace_id } : {}),
        repository: repository.id,
        revision,
        target: `${repository.name}:latest`,
      });
      navigate(repository.workspace_id ? `/w/${encodeURIComponent(repository.workspace_id)}/ship/packages` : `/ship/packages`);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <section className="panel">
      <div className="section-heading">
        <div>
          <h2>Build Image</h2>
          <p className="muted">Queue an in-cluster build from this repository.</p>
        </div>
        <Button variant="primary" loading={busy} onClick={() => void build()}>
          Build image
        </Button>
      </div>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
