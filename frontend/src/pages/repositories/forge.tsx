/** What the repository, forge and Drive pages share: the shapes their capabilities answer, lake's
 * forge markup helpers (identifiers, badges, sizes, the repository tabs and breadcrumbs), the
 * repository error page and the conflict resolution block (web_ui.cpp). */
import { type ReactNode, useState } from "react";
import { Link, Navigate, useLocation } from "react-router-dom";
import { useQuery, type UseQueryOptions, type UseQueryResult } from "@tanstack/react-query";
import { act, ApiError, query, read, useRead, type Params } from "@/lib/api";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Badge, Button, cx, Skeleton, type Tone } from "@/lib/ui";

/* ------------------------------------------------------------------ calls */

/** Repository pages are not workspace pages: they keep their addresses and name the workspace
 * (docs/ui/repositories.md). Their capabilities are called without the `/w/<id>` prefix, which
 * would otherwise fill and pin `workspace_id` to the remembered workspace. */
export const UNSCOPED = { workspace: null } as const;

export function useForgeRead<T>(name: string | null, input?: Params, options?: Partial<UseQueryOptions<T>>) {
  return useRead<T>(name, input, { ...options, ...UNSCOPED });
}

export function forgeAct<T = Record<string, unknown>>(name: string, input: Record<string, unknown>) {
  return act<T>(name, input, UNSCOPED);
}

/** The rows of an `{items: [...]}` answer. */
export function itemsOf<T>(value: { items?: T[] } | undefined | null): T[] {
  return value?.items ?? [];
}

/* ------------------------------------------------------------------ shapes */

export type Repository = {
  id: string;
  customer_id: string;
  workspace_id: string | null;
  name: string;
  description: string;
  visibility: string;
  deleted_at: string | null;
  retention_until: string | null;
  created_at: string;
  updated_at: string;
  description_unavailable?: string;
};
export type Items<T> = { items: T[] };
export type Ref = { name: string; change: string };
export type TreeEntry = { path: string; kind: string; blob: string; mode: number; node_id: string; size: number; stored_size: number; modified_at?: string };
export type Change = { id: string; parents: string[]; author: string; message: string; occurred_at: string };
export type Hunk = { old_line: number; old_count: number; new_line: number; new_count: number; removed_lines: string[]; added_lines: string[] };
export type DiffEntry = {
  path: string;
  old_path: string;
  kind: string;
  changes: string[];
  old_mode: number | null;
  new_mode: number | null;
  old_blob: string;
  new_blob: string;
  line_hunks: Hunk[] | null;
  old_conflicts: string[];
  new_conflicts: string[];
};
/** A forge record (issue, review, release, webhook, …): its fields are in `value`. */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type ForgeRecord = { id: string; repository_id: string; kind: string; value: Record<string, any>; created_at: string; updated_at: string };
export type ArtifactOutput = { path: string; mode: number; blob: string };
export type Attestation = { id: string; profile: string; subject: string; issuer: string; verified: boolean; reason: string };
export type ArtifactMember = { artifact: string; version: string; path: string; digest: string; indexed: boolean; reason: string };
export type Artifact = {
  id: string;
  action: string;
  exit_code: number;
  outputs: ArtifactOutput[];
  timing: { queued_ms: number; sandbox_ms: number; exec_ms: number; hash_ms: number; store_ms: number };
  cache_status: string;
  divergent: boolean;
  verification: string;
  worker: string;
  observed_at: string;
  started_at: string;
  finished_at: string;
  attestations: Attestation[];
  trace: { id: string; wall_ms: number; actions: number; dependencies: number; cache_hits: number; cache_misses: number } | null;
  members: ArtifactMember[];
};
export type Conflict = {
  id: string;
  base: string;
  left: string;
  right: string;
  region: { path: string; bytes?: { offset: number; length: number }; nodes?: { first: string; last: string } };
  kind: string;
  explanation: string;
  reasons: string[];
  proposal?: string;
  node?: string;
};
export type AccessEntry = { principal_taap_id: string; source_object_id: string; role: string; actions: string[]; inherited: boolean };
export type PolicyObject = { id: string; object_type: string; external_id: string };

/* ------------------------------------------------------------------ text */

export function repositoryName(repository: Pick<Repository, "id" | "name">): string {
  return repository.name || repository.id;
}

/** lake's `abbreviated_identifier`: 20 characters or fewer stay whole, else the first 12, "…",
 * the last 7. */
export function abbreviated(value: string): string {
  return value.length <= 20 ? value : `${value.slice(0, 12)}…${value.slice(-7)}`;
}

/** An identifier in mono, abbreviated, with the whole value on hover (`identifier_markup`). */
export function Identifier({ value }: { value: string | null | undefined }) {
  const full = value ?? "";
  return (
    <code className="identifier" title={full} data-full-identifier={full}>
      {abbreviated(full)}
    </code>
  );
}

export function IdentifierList({ values, empty = "None" }: { values: string[] | undefined; empty?: string }) {
  if (!values?.length) return <span className="muted">{empty}</span>;
  return (
    <span className="identity-list">
      {values.map((value, index) => (
        <Identifier key={index} value={value} />
      ))}
    </span>
  );
}

/** lake's `badge_markup`: the colour a forge value carries. */
export function forgeTone(value: string): Tone {
  if (["active", "success", "verified", "hit", "added"].includes(value)) return "success";
  if (["revoked", "denied", "failed", "divergent", "removed", "conflicted"].includes(value)) return "danger";
  if (["pending", "warning", "miss"].includes(value)) return "warning";
  if (["model", "agent", "modified"].includes(value)) return "ai";
  return "neutral";
}

export function ForgeBadge({ value }: { value: string }) {
  return <Badge tone={forgeTone(value)}>{value}</Badge>;
}

/** `byte_count`: the forge pages' sizes. */
export function byteCount(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

/** `display_size`: the Drive pages' sizes. */
export function displaySize(size: number): string {
  const units = ["B", "KiB", "MiB", "GiB"];
  let value = size;
  let unit = 0;
  while (value >= 1024 && unit + 1 < units.length) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${size} ${units[0]}` : `${value.toFixed(1)} ${units[unit]}`;
}

export function octal(mode: number | null | undefined): string {
  return mode === null || mode === undefined ? "—" : mode.toString(8);
}

export function parentPath(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

export function pathName(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? path : path.slice(slash + 1);
}

/** lake's `markdown_slug`, used for the diff anchors (`#diff-<slug>`). */
export function slug(heading: string): string {
  let out = "";
  let pending = false;
  for (const c of heading) {
    if (/^[A-Za-z0-9]$/.test(c)) {
      if (pending && out) out += "-";
      out += c.toLowerCase();
      pending = false;
    } else if (out) {
      pending = true;
    }
  }
  return out || "section";
}

/** `file_language`. */
export function fileLanguage(path: string): string {
  const dot = path.lastIndexOf(".");
  if (dot < 0) return "Other";
  const extension = path.slice(dot);
  if ([".c", ".h"].includes(extension)) return "C";
  if ([".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"].includes(extension)) return "C++";
  if ([".md", ".markdown"].includes(extension)) return "Markdown";
  if (extension === ".json") return "JSON";
  if ([".js", ".mjs"].includes(extension)) return "JavaScript";
  if ([".ts", ".tsx"].includes(extension)) return "TypeScript";
  if (extension === ".py") return "Python";
  if (extension === ".rs") return "Rust";
  return "Other";
}

/** UTF-8 text → padded base64 (`resolve_conflict`'s prepared text). */
export function textToBase64(text: string): string {
  return bytesToBase64(new TextEncoder().encode(text));
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  return btoa(binary);
}

/* ------------------------------------------------------------------ links */

/** A Drive address: `/drive?repository_id=&ref=&path=` (and the other Drive pages). */
export function driveHref(page: string, repositoryId: string, ref: string, path: string, extra: Params = {}): string {
  return `${page}?repository_id=${encodeURIComponent(repositoryId)}&ref=${encodeURIComponent(ref)}&path=${encodeURIComponent(path)}${query(extra).replace(/^\?/, "&")}`;
}

/* ------------------------------------------------------------------ page structure */

/** A forge page: what sits above the heading (the path breadcrumbs), the eyebrow, `<h1>`, sub
 * line and actions, then the content. Lake's page classes. */
export function ForgePage({
  above,
  eyebrow,
  title,
  sub,
  actions,
  children,
}: {
  above?: ReactNode;
  eyebrow: ReactNode;
  title: ReactNode;
  sub?: ReactNode;
  actions?: ReactNode;
  children?: ReactNode;
}) {
  return (
    <div className="page-container">
      {above}
      <header className="page-header">
        <div className="min-w-0">
          <p className="eyebrow">{eyebrow}</p>
          <h1>{title}</h1>
          {sub && <p className="sub">{sub}</p>}
        </div>
        {actions && <div className="page-actions">{actions}</div>}
      </header>
      {children}
    </div>
  );
}

/** The `repository_shell` trail: Repositories › <repository> › <page>. */
export function useRepositoryTrail(repository: Pick<Repository, "id" | "name"> | undefined, page: string) {
  useTrail(
    repository
      ? [{ label: "Repositories", to: "/repositories" }, entityCrumb(repositoryName(repository), `/repository?id=${encodeURIComponent(repository.id)}`), { label: page }]
      : null,
  );
}

type TabKey = "files" | "code" | "changes" | "issues" | "releases" | "artifacts" | "platform" | "webhooks" | "protection" | "conflicts";

/** `forge_tabs`: the repository's sections. Signed out, Files, Deployments and Webhooks are
 * hidden; Protection is shown to the operator only, because its page reads the instance audit. */
export function ForgeTabs({ repository, current }: { repository: Pick<Repository, "id">; current: TabKey }) {
  const { viewer } = useSession();
  const id = encodeURIComponent(repository.id);
  const tabs: [string, string, TabKey][] = [
    ["Files", `/drive?repository_id=${id}`, "files"],
    ["Code", `/repository?id=${id}`, "code"],
    ["Changes", `/changes?repository_id=${id}`, "changes"],
    ["Issues", `/issues?repository_id=${id}`, "issues"],
    ["Releases", `/releases?repository_id=${id}`, "releases"],
    ["Artifacts", `/artifacts?repository_id=${id}`, "artifacts"],
    ["Deployments", `/environments?repository_id=${id}`, "platform"],
    ["Webhooks", `/webhooks?repository_id=${id}`, "webhooks"],
    ["Protection", `/protection?repository_id=${id}`, "protection"],
  ];
  return (
    <nav className="tabs forge-tabs" aria-label="Repository">
      {tabs
        .filter(([, , key]) => viewer.signed_in || !["files", "platform", "webhooks"].includes(key))
        .filter(([, , key]) => key !== "protection" || viewer.administrator)
        .map(([label, to, key]) => (
          <Link key={key} to={to} aria-current={key === current ? "page" : undefined}>
            {label}
          </Link>
        ))}
    </nav>
  );
}

/** `path_breadcrumbs`: <repository> / <ref> / dir… / <file>. */
export function PathBreadcrumbs({ repository, refName, path }: { repository: Pick<Repository, "id" | "name">; refName: string; path: string }) {
  const id = encodeURIComponent(repository.id);
  const tree = `/repository/tree?id=${id}&ref=${encodeURIComponent(refName)}`;
  const parts = path ? path.split("/") : [];
  return (
    <nav className="breadcrumbs" aria-label="Breadcrumb">
      <Link to={`/repository?id=${id}`}>{repositoryName(repository)}</Link>
      <span aria-hidden="true">/</span>
      <Link to={tree}>{refName}</Link>
      {parts.map((part, index) => {
        const accumulated = parts.slice(0, index + 1).join("/");
        return (
          <span key={index} className="contents">
            <span aria-hidden="true">/</span>
            {index === parts.length - 1 ? <span aria-current="page">{part}</span> : <Link to={`${tree}#${encodeURIComponent(accumulated)}`}>{part}</Link>}
          </span>
        );
      })}
    </nav>
  );
}

/** `drive_breadcrumbs`: <repository> / folder… / <current>. */
export function DriveBreadcrumbs({ repository, refName, path }: { repository: Pick<Repository, "id" | "name">; refName: string; path: string }) {
  const parts = path ? path.split("/") : [];
  return (
    <nav className="breadcrumbs" aria-label="Breadcrumb">
      <Link to={driveHref("/drive", repository.id, refName, "")}>{repositoryName(repository)}</Link>
      {parts.map((part, index) => (
        <span key={index} className="contents">
          <span aria-hidden="true">/</span>
          {index === parts.length - 1 ? (
            <span aria-current="page">{part}</span>
          ) : (
            <Link to={driveHref("/drive", repository.id, refName, parts.slice(0, index + 1).join("/"))}>{part}</Link>
          )}
        </span>
      ))}
    </nav>
  );
}

/* ------------------------------------------------------------------ errors */

/** `serve_repository_page_error`: a signed-out visitor refused a repository is sent to sign in,
 * so a private repository's existence is not disclosed; anyone else gets lake's error page. */
export function RepositoryError({ error }: { error: unknown }) {
  const { viewer } = useSession();
  const location = useLocation();
  const e = error instanceof ApiError ? error : null;
  if (!viewer.signed_in && e && (e.code === "unauthorized" || e.code === "unauthenticated" || e.status === 401)) {
    return <Navigate to={`/sign-in?next=${encodeURIComponent(location.pathname + location.search)}`} replace />;
  }
  const code = e?.code ?? "error";
  const message = e?.message ?? (error instanceof Error ? error.message : "The request failed.");
  return <ErrorPage code={code} message={message} />;
}

/** `web_ui_error`: eyebrow the code, "We Could Not Load This Page", the message, the way back. */
export function ErrorPage({ code, message }: { code: string; message: string }) {
  useTrail([{ label: code }], "error");
  return (
    <div className="page-container">
      <header className="page-header">
        <div className="min-w-0">
          <p className="eyebrow">{code}</p>
          <h1>We Could Not Load This Page</h1>
          <p className="sub">{message}</p>
        </div>
      </header>
      <Button to="/repositories">Back to repositories</Button>
    </div>
  );
}

/** Loading or the first failure among a page's required reads, else null (the page renders). */
export function pending(...queries: UseQueryResult<unknown>[]): ReactNode | null {
  const failed = queries.find((q) => q.isError);
  if (failed) return <RepositoryError error={failed.error} />;
  if (queries.some((q) => q.isLoading || q.data === undefined)) {
    return (
      <div className="page-container">
        <Skeleton />
      </div>
    );
  }
  return null;
}

/** A required query parameter is missing: lake sends the viewer to the repository list. */
export function ToRepositories() {
  return <Navigate to="/repositories" replace />;
}

/* ------------------------------------------------------------------ conflicts */

/** `conflict_kind_label`. */
export function conflictKindLabel(kind: string): string {
  if (kind === "text") return "Text overlap";
  if (kind === "structural") return "Structural overlap";
  if (kind === "crdt-suspicious") return "Suspicious concurrent update";
  if (kind === "suspicious-merge") return "Suspicious merge proposal";
  return "Conflict";
}

export function ConflictRegion({ region }: { region: Conflict["region"] }) {
  return (
    <>
      <code>{region.path}</code>
      {region.bytes && ` · byte offset ${region.bytes.offset} · length ${region.bytes.length}`}
      {region.nodes && (
        <>
          {" · nodes "}
          <Identifier value={region.nodes.first} />
          {" through "}
          <Identifier value={region.nodes.last} />
        </>
      )}
    </>
  );
}

export function ConflictReasons({ conflict }: { conflict: Conflict }) {
  if (!conflict.reasons?.length) return null;
  return (
    <>
      <h3>Why This Needs Attention</h3>
      <ul>
        {conflict.reasons.map((reason, index) => (
          <li key={index}>{reason}</li>
        ))}
      </ul>
    </>
  );
}

export function ConflictValues({ conflict }: { conflict: Conflict }) {
  return (
    <dl>
      <dt>Base value</dt>
      <dd>
        <Identifier value={conflict.base} />
      </dd>
      <dt>Left value</dt>
      <dd>
        <Identifier value={conflict.left} />
      </dd>
      <dt>Right value</dt>
      <dd>
        <Identifier value={conflict.right} />
      </dd>
      {conflict.proposal && (
        <>
          <dt>Reviewed proposal</dt>
          <dd>
            <Identifier value={conflict.proposal} />
          </dd>
        </>
      )}
    </dl>
  );
}

/** `conflict_resolution_markup`: keep left, keep right, accept the reviewed proposal, or use
 * prepared text (sent base64, as the form handler encoded it). `done` is where the handler
 * redirected after a success. */
export function ConflictResolution({
  repositoryId,
  refName,
  conflict,
  done,
}: {
  repositoryId: string;
  refName: string;
  conflict: Conflict;
  done: () => void;
}) {
  const { viewer } = useSession();
  const [text, setText] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<unknown>(null);
  if (!viewer.signed_in) return <p className="sub">Sign in with write access to resolve this conflict.</p>;
  const resolve = async (resolution: "left" | "right" | "accept_proposal" | "blob") => {
    setBusy(resolution);
    setError(null);
    try {
      await forgeAct("resolve_conflict", {
        repository_id: repositoryId,
        ref: refName,
        path: conflict.region.path,
        conflict_id: conflict.id,
        resolution,
        ...(resolution === "blob" ? { blob: textToBase64(text) } : {}),
      });
      done();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(null);
    }
  };
  return (
    <>
      <h3>Choose the Value to Keep</h3>
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void resolve("blob");
        }}
      >
        {error ? (
          <p className="error" role="alert">
            {(error as Error).message}
          </p>
        ) : null}
        <div className="actions">
          <Button loading={busy === "left"} disabled={!!busy} onClick={() => void resolve("left")}>
            Keep left value
          </Button>
          <Button loading={busy === "right"} disabled={!!busy} onClick={() => void resolve("right")}>
            Keep right value
          </Button>
          {conflict.proposal && (
            <Button variant="primary" loading={busy === "accept_proposal"} disabled={!!busy} onClick={() => void resolve("accept_proposal")}>
              Accept reviewed proposal
            </Button>
          )}
        </div>
        <label>
          Prepared replacement text
          <textarea rows={6} value={text} onChange={(e) => setText(e.target.value)} />
        </label>
        <div>
          <Button type="submit" loading={busy === "blob"} disabled={!!busy}>
            Use prepared text
          </Button>
        </div>
      </form>
    </>
  );
}

/** `drive_diff_markup`: one line per span, "-" removed, "+" added, " " unchanged. */
export function DriveDiff({ lines }: { lines: { kind: string; text: string }[] }) {
  return (
    <pre className="diff-hunk drive-version-diff">
      {lines.map((line, index) => (
        <span key={index} className={cx(line.kind === "removed" && "diff-remove", line.kind === "added" && "diff-add")}>
          {(line.kind === "removed" ? "-" : line.kind === "added" ? "+" : " ") + line.text}
        </span>
      ))}
    </pre>
  );
}

/* ------------------------------------------------------------------ history */

/** Every change's diff, newest first: what the tree page's "last change" and the file page's
 * blame are computed from. lake has no "last change per path" read, so the page makes the same
 * `change_diff` call per change the server made, a few at a time. */
export function useChangeDiffs(id: string, changes: Change[] | undefined) {
  const ids = changes?.map((change) => change.id) ?? [];
  return useQuery<DiffEntry[][]>({
    queryKey: ["change_diff*", id, ids],
    enabled: changes !== undefined,
    queryFn: async ({ signal }) => {
      const out: DiffEntry[][] = new Array(ids.length);
      let next = 0;
      const worker = async () => {
        while (next < ids.length) {
          const index = next++;
          const value = await read<Items<DiffEntry>>("change_diff", { id, change_id: ids[index] }, { ...UNSCOPED, signal });
          out[index] = value.items ?? [];
        }
      };
      await Promise.all(Array.from({ length: Math.min(6, ids.length) }, worker));
      return out;
    },
  });
}
