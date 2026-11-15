// SPDX-License-Identifier: MIT

/** /repository?id= (web_ui_repository): the repository home, as a code host draws it. A header with
 * the name, visibility and actions (Clone, Build image); the repository's tabs; figures; then the
 * files at the default ref, recent changes and the README on the left, and clone and push, About,
 * references, releases, languages and effective access on the right. Public repositories render
 * for visitors. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { BookOpen, Download, FolderTree, GitCommitHorizontal, Hammer, History, Package, Tag } from "lucide-react";
import { ApiError } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Modal } from "@/lib/overlay";
import { Banner, Button, Card, Chip, CopyBlock, Empty, KeyValue, Meter, Split, Stat, StatRow, Status } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { Markdown } from "./Markdown";
import {
  byteCount,
  EntryIcon,
  fileLanguage,
  ForgeStatus,
  ForgeTabs,
  forgeAct,
  Identifier,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  Visibility,
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
/** `contents` is absent when the file is not UTF-8 text (lake#1238). */
type RepositoryFile = { path: string; contents?: string };

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
  const [cloning, setCloning] = useState(false);

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
  const releaseList = itemsOf(releases.data);
  const blobs = entries.filter((entry) => entry.kind === "blob");
  const languages = new Map<string, number>();
  for (const entry of blobs) languages.set(fileLanguage(entry.path), (languages.get(fileLanguage(entry.path)) ?? 0) + entry.size);
  const total = blobs.reduce((sum, entry) => sum + entry.size, 0);
  const encoded = encodeURIComponent(id);
  const refQuery = encodeURIComponent(defaultRef);
  const origin = viewer.external_origin ?? "";
  const url = shellArgument(`${origin}/v1/repositories/${id}`);
  const directory = shellArgument(`./${r.name}`);
  const treeHref = `/repository/tree?id=${encoded}&ref=${refQuery}`;

  const clone = (
    <>
      {!origin ? (
        <p className="muted m-0">
          An administrator must set <code>LAKE_EXTERNAL_ORIGIN</code> or <code>LAKE_PUBLIC_HOST</code> before clone and push commands are available.
        </p>
      ) : (
        <div className="space-y-3">
          <p className="muted m-0 text-[12.5px]">
            Create an account token on this instance under <Link to="/tokens">Tokens</Link> and export it as <code>LIQUID_TOKEN</code> in your terminal. Use a token issued by this
            instance only.
          </p>
          <CopyBlock label="First checkout into a new directory" value={`liquid clone ${url} ${directory}\ncd ${directory}`} multiline />
          <CopyBlock label="From that checkout, after capturing and naming your changes" value={`liquid push ${url}`} />
        </div>
      )}
    </>
  );

  return (
    <div className="page-container">
      <header className="page-header">
        <div className="min-w-0">
          <p className="eyebrow">Data · Repository</p>
          <div className="flex flex-wrap items-center gap-2">
            <h1 className="break-words">{title}</h1>
            <Visibility value={r.visibility} />
            {r.deleted_at && <Status value="deleted" />}
          </div>
          <p className="sub">{r.description_unavailable ? `Description unavailable: ${r.description_unavailable}` : r.description || "No description."}</p>
          <div className="mt-1 flex flex-wrap items-center gap-x-4 gap-y-1 text-[12.5px] text-ink-2">
            <span>
              Owner <Link to={`/repositories?customer_id=${encodeURIComponent(r.customer_id)}`}>{ownerName}</Link>
            </span>
            {workspace.data && (
              <span>
                Workspace <Link to={`/w/${encodeURIComponent(workspace.data.id)}/design/projects`}>{workspace.data.name}</Link>
              </span>
            )}
            <span>Updated {relative(r.updated_at)}</span>
          </div>
        </div>
        <div className="page-actions">
          <Button to={treeHref}>
            <FolderTree size={14} aria-hidden /> Browse files
          </Button>
          {signedIn && !r.deleted_at && <BuildImage repository={r} revision={defaultRef} />}
          <Button variant="primary" onClick={() => setCloning(true)}>
            <Download size={14} aria-hidden /> Clone
          </Button>
        </div>
      </header>
      <ForgeTabs repository={r} current="code" />
      {signedIn && !r.workspace_id && (
        <Banner tone="quiet" title="No workspace yet">
          This repository was created before workspaces and has not been assigned one.
        </Banner>
      )}
      {r.deleted_at && (
        <Banner tone="warn" title="Tombstoned">
          Deleted {dateTime(r.deleted_at)}. Collection may reclaim unreferenced content after {dateTime(r.retention_until)}.
        </Banner>
      )}
      <StatRow>
        <Stat label="Files" value={hasRefs ? blobs.length : 0} icon={FolderTree} sub={hasRefs ? `${byteCount(total)} at ${defaultRef}` : "nothing published yet"} to={treeHref} />
        <Stat label="Changes" value={recent.length} icon={GitCommitHorizontal} sub={recent[0] ? `latest ${relative(recent[0].occurred_at)}` : "none cut yet"} to={`/changes?repository_id=${encoded}`} />
        <Stat label="References" value={refList.length} icon={Tag} sub={hasRefs ? `default ${defaultRef}` : "none published"} />
        <Stat
          label="Releases"
          value={releases.isError ? "—" : releaseList.length}
          icon={Package}
          sub={releases.isError ? "unavailable" : releaseList[0]?.value.tag ? `latest ${releaseList[0].value.tag}` : "none published"}
          to={`/releases?repository_id=${encoded}`}
        />
      </StatRow>
      <Split
        side={
          <>
            <Card label="Clone and push" title="Use liquid">
              {clone}
            </Card>
            <Card label="About">
              <KeyValue
                rows={[
                  ["Identifier", <Identifier value={r.id} />],
                  ["Owner", <Link to={`/repositories?customer_id=${encodeURIComponent(r.customer_id)}`}>{ownerName}</Link>],
                  ["Workspace", workspace.data ? <Link to={`/w/${encodeURIComponent(workspace.data.id)}/design/projects`}>{workspace.data.name}</Link> : null],
                  ["Visibility", <Visibility value={r.visibility} />],
                  ["Created", dateTime(r.created_at)],
                  ["Updated", dateTime(r.updated_at)],
                ]}
              />
            </Card>
            <Card label="References" actions={<span className="font-mono text-[11px] text-ink-3">{refList.length}</span>}>
              {refList.length === 0 ? (
                <p className="muted m-0">No references are published.</p>
              ) : (
                <ul className="detail-list">
                  {refList.map((ref, index) => (
                    <li key={ref.name}>
                      <span className="flex min-w-0 items-center gap-1.5">
                        <Link to={`/repository/tree?id=${encoded}&ref=${encodeURIComponent(ref.name)}`}>{ref.name}</Link>
                        {index === 0 && <Chip tone="action">default</Chip>}
                      </span>
                      <Identifier value={ref.change} />
                    </li>
                  ))}
                </ul>
              )}
            </Card>
            <Card label="Releases" actions={<Link to={`/releases?repository_id=${encoded}`}>All releases</Link>}>
              {releases.isError ? (
                <p className="muted m-0">Releases are unavailable: {(releases.error as Error).message}</p>
              ) : releaseList.length === 0 ? (
                <p className="muted m-0">No releases have been published.</p>
              ) : (
                <ul className="detail-list">
                  {releaseList.map((release) => (
                    <li key={release.id}>
                      <span className="flex items-center gap-1.5">
                        <Tag size={13} aria-hidden className="text-ink-3" />
                        <strong>{release.value.tag ?? "untagged"}</strong>
                      </span>
                      <Identifier value={release.value.change_id ?? ""} />
                    </li>
                  ))}
                </ul>
              )}
            </Card>
            <Card label="Languages">
              {languages.size === 0 ? (
                <p className="muted m-0">No indexed file sizes yet.</p>
              ) : (
                <>
                  <ul className="m-0 list-none space-y-2 p-0">
                    {[...languages.entries()]
                      .sort(([, a], [, b]) => b - a)
                      .map(([language, size]) => (
                        <li key={language}>
                          <div className="mb-0.5 text-[12.5px] text-ink">{language}</div>
                          <Meter value={size} max={total || 1} label={byteCount(size)} />
                        </li>
                      ))}
                  </ul>
                  <p className="muted mt-2 mb-0 text-[12px]">
                    {byteCount(total)} indexed across {blobs.length} files.
                  </p>
                </>
              )}
            </Card>
            <Card label="Effective access" actions={signedIn ? <Link to={`/repository/access?id=${encoded}`}>View access details</Link> : undefined}>
              {access.isError ? (
                <p className="muted m-0">Effective access is unavailable: {(access.error as Error).message}</p>
              ) : itemsOf(access.data).length === 0 ? (
                <p className="muted m-0">No direct or inherited grants are visible.</p>
              ) : (
                <ul className="detail-list">
                  {itemsOf(access.data).map((entry, index) => (
                    <li key={index}>
                      <Identifier value={entry.principal_taap_id} />
                      <ForgeStatus value={entry.role} />
                    </li>
                  ))}
                </ul>
              )}
            </Card>
          </>
        }
      >
        <Card
          padded={false}
          label="Files"
          title={
            <>
              The published tree at <code>{defaultRef}</code>
            </>
          }
          actions={<Button to={treeHref}>Browse tree</Button>}
        >
          {entries.length === 0 ? (
            <div className="p-4">
              <Empty compact icon={FolderTree} title="No files yet">
                No files are published on this ref. Push a change to publish its first tree.
              </Empty>
            </div>
          ) : (
            <ul className="m-0 list-none divide-y divide-hairline p-0">
              {entries.slice(0, 12).map((entry) => (
                <li key={entry.path} className="flex items-center gap-2.5 px-4 py-2 text-[13px]">
                  <EntryIcon kind={entry.kind === "blob" ? "blob" : "tree"} size={14} />
                  <Link
                    className="min-w-0 flex-1 truncate"
                    to={
                      entry.kind === "blob"
                        ? `/file?repository_id=${encoded}&ref=${refQuery}&path=${encodeURIComponent(entry.path)}`
                        : `/repository/tree?id=${encoded}&ref=${refQuery}#${encodeURIComponent(entry.path)}`
                    }
                  >
                    {entry.path}
                  </Link>
                  <span className="font-mono text-[11.5px] text-ink-3">{entry.kind === "blob" ? byteCount(entry.size) : "directory"}</span>
                </li>
              ))}
              {entries.length > 12 && (
                <li className="px-4 py-2 text-[12.5px]">
                  <Link to={treeHref}>All {entries.length} paths</Link>
                </li>
              )}
            </ul>
          )}
        </Card>
        <Card padded={false} label="Recent changes" actions={<Link to={`/changes?repository_id=${encoded}`}>All changes</Link>}>
          {recent.length === 0 ? (
            <div className="p-4">
              <Empty compact icon={History} title="No changes yet">
                No changes have been cut.
              </Empty>
            </div>
          ) : (
            <ol className="m-0 list-none divide-y divide-hairline p-0">
              {recent.slice(0, 5).map((change) => (
                <li key={change.id} className="flex items-start gap-3 px-4 py-2.5">
                  <GitCommitHorizontal size={15} aria-hidden className="mt-0.5 shrink-0 text-ink-3" />
                  <div className="min-w-0 flex-1">
                    <Link className="font-semibold" to={`/change?repository_id=${encoded}&id=${encodeURIComponent(change.id)}`}>
                      {change.message.split("\n")[0] || "Untitled change"}
                    </Link>
                    <div className="truncate text-[12px] text-ink-3">
                      {change.author} · <span title={dateTime(change.occurred_at)}>{relative(change.occurred_at)}</span>
                    </div>
                  </div>
                  <Identifier value={change.id} />
                </li>
              ))}
            </ol>
          )}
        </Card>
        <Card label="README" title={readme.data ? "README.md" : undefined}>
          {readme.data ? (
            <div className="readme-panel">
              <Markdown>{readme.data.contents ?? ""}</Markdown>
            </div>
          ) : (
            <Empty compact icon={BookOpen} title="No README">
              Add a README.md to explain this repository.
            </Empty>
          )}
        </Card>
      </Split>
      {cloning && (
        <Modal title="Clone This Repository" subtitle={title} onClose={() => setCloning(false)} width={640}>
          {clone}
        </Modal>
      )}
    </div>
  );
}

/** "Build image": queue an in-cluster build of the default ref as `<name>:latest`; success opens
 * the workspace's Packages page, where the build shows (serve_repository_build). A refusal is
 * said in a toast. */
function BuildImage({ repository, revision }: { repository: Repository; revision: string }) {
  const navigate = useNavigate();
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const build = async () => {
    setBusy(true);
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
      toast(`Build image: ${(e as Error).message}`, "bad");
    } finally {
      setBusy(false);
    }
  };
  return (
    <Button loading={busy} onClick={() => void build()} title="Queue an in-cluster build from this repository">
      {!busy && <Hammer size={14} aria-hidden />} Build image
    </Button>
  );
}
