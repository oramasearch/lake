/** Secrets (`/secrets?mount_id=` and `/secrets?mount_id=&name=`), docs/ui/secrets.md.
 *
 * - With only `mount_id`, the mount's page: its secrets from `list_secrets` (paths and metadata,
 *   never values), search, and the New Secret drawer (`put_secret`).
 * - With `name`, one secret's page: its versions (`secret_metadata`), New Version (`put_secret`
 *   with check-and-set), a deliberate Reveal (`read_secret`), and delete, undelete and destroy.
 *
 * A value is held only in component state: never in the URL, the query cache, a log or storage. */
import { useState } from "react";
import { Link, Navigate, useNavigate } from "react-router-dom";
import { ArrowLeft, ChevronLeft, ChevronRight, Eraser, Eye, EyeOff, FileKey, History, KeyRound, Lock, Plus, Trash2, Undo2, Vault } from "lucide-react";
import { act, read, useRead, type Row } from "@/lib/api";
import { plural } from "@/lib/format";
import { ConfirmButton } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, DataTable, Empty, ErrorBlock, FilterChips, KeyValue, Mono, Page, SearchInput, Skeleton, Split, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { RevealedValue, WriteSecretDrawer } from "./secretParts";
import { accountLabel } from "./secretOwners";
import { versionState, type VersionState } from "./secretValue";

const PAGE = 200;
const STATE_TONE: Record<VersionState, "success" | "warning" | "danger"> = { active: "success", deleted: "warning", destroyed: "danger" };

export default function SecretsPage() {
  const mountId = useParam("mount_id");
  const name = useParam("name");
  const mount = useRead<Row>(mountId ? "get_secret_mount" : null, { mount_id: mountId });
  useMetaTrail(name || mount.data?.name, "Secrets");
  if (!mountId) return <Navigate to="/secret-mounts" replace />;
  if (mount.isError) {
    return (
      <Page eyebrow="Organization" title="Secrets">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="Organization" title="Secrets">
        <Skeleton lines={5} />
      </Page>
    );
  }
  // Keyed by the secret, so a value revealed on one page never shows on another.
  return name ? <SecretView key={name} mount={mount.data} name={name} /> : <MountView mount={mount.data} />;
}

function secretPath(mountId: string, name: string) {
  return `/secrets?mount_id=${encodeURIComponent(mountId)}&name=${encodeURIComponent(name)}`;
}

function MountFacts({ mount }: { mount: Row }) {
  const session = useSession();
  const workspace = session.workspaces.find((w) => w.id === mount.workspace_id);
  return (
    <Card label="Mount" title={mount.name}>
      <KeyValue
        rows={[
          ["Description", mount.description || null],
          ["Workspace", workspace ? <span title={mount.workspace_id}>{workspace.name}</span> : <Mono short={20}>{mount.workspace_id}</Mono>],
          ["Owner", mount.organization_id ? <OrganizationName id={mount.organization_id} /> : accountLabel(session.viewer.spaces.find((s) => s.id === mount.customer_id))],
          ["Created", <When value={mount.created_at} />],
          ["Mount id", <Mono short={20}>{mount.id}</Mono>],
        ]}
      />
      <p className="muted mb-0 flex flex-wrap gap-3 text-[12.5px]">
        <Link to={`/encryption-keys?mount_id=${encodeURIComponent(mount.id)}`}>Encryption keys</Link>
        <Link to={`/secret-mounts?workspace_id=${encodeURIComponent(mount.workspace_id)}`}>All secret mounts</Link>
      </p>
    </Card>
  );
}

/** An organization that owns a mount, by name when the viewer can read it. */
function OrganizationName({ id }: { id: string }) {
  const organization = useRead<Row>("get_organization", { id }, { workspace: null, retry: false });
  return organization.data ? <span title={id}>{organization.data.display_name || organization.data.name || id}</span> : <Mono short={20}>{id}</Mono>;
}

/* ------------------------------------------------------------------ a mount's secrets */

function MountView({ mount }: { mount: Row }) {
  const navigate = useNavigate();
  // The cursors of the pages before this one, then this page's.
  const [cursors, setCursors] = useState<string[]>([""]);
  const cursor = cursors[cursors.length - 1];
  const secrets = useRead<{ items: Row[]; next_cursor?: string }>("list_secrets", { mount_id: mount.id, limit: PAGE, cursor: cursor || undefined });
  const [search, setSearch] = useState("");
  const [state, setState] = useState<"live" | "deleted" | null>(null);
  const [writing, setWriting] = useState(false);
  const items = secrets.data?.items;
  const deleted = items?.filter((s) => s.deleted).length ?? 0;
  const rows = items?.filter((s) => matches(search, s.name) && (!state || (state === "deleted") === !!s.deleted));
  const existing = new Map((items ?? []).map((s) => [s.name as string, Number(s.current_version)]));
  const newSecret = (
    <Button variant="primary" onClick={() => setWriting(true)}>
      <Plus size={14} aria-hidden /> New secret
    </Button>
  );
  const paged = cursors.length > 1 || !!secrets.data?.next_cursor;
  return (
    <Page
      eyebrow="Organization"
      title={mount.name}
      sub={mount.description || "Versioned JSON secrets, sealed in this mount's workspace. Values are shown only when you reveal one."}
      actions={
        <>
          <Button to={`/encryption-keys?mount_id=${encodeURIComponent(mount.id)}`}>
            <KeyRound size={14} aria-hidden /> Encryption keys
          </Button>
          {newSecret}
        </>
      }
    >
      {items && items.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Secrets" value={paged ? `${items.length}+` : items.length} icon={Lock} sub={paged ? "on this page" : "in this mount"} />
          <Stat label="Live" value={items.length - deleted} sub="current version readable" tone="ok" />
          <Stat label="Deleted" value={deleted} sub="current version deleted or destroyed" />
        </StatRow>
      )}
      <Split side={<MountFacts mount={mount} />}>
        <div>
          {(items?.length ?? 0) > 0 && (
            <Toolbar>
              <SearchInput value={search} onChange={setSearch} placeholder="Search paths…" label="Search secrets" />
              <FilterChips
                options={[
                  { id: "live" as const, label: "Live", count: items ? items.length - deleted : undefined },
                  { id: "deleted" as const, label: "Deleted", count: items ? deleted : undefined },
                ]}
                value={state}
                onChange={setState}
              />
            </Toolbar>
          )}
          <DataTable
            q={secrets}
            what="secrets"
            rows={rows}
            rowKey={(s) => s.name}
            empty={
              items?.length ? (
                <PanelEmpty title="No secret matches">Clear the search or the filter to see every secret.</PanelEmpty>
              ) : (
                <Empty icon={Vault} title="No secrets yet" action={newSecret}>
                  Write the first secret: a path such as <code>database/password</code> and a JSON value.
                </Empty>
              )
            }
            columns={[
              {
                key: "name",
                label: "Secret",
                sort: (s) => s.name,
                render: (s) => <NameCell to={secretPath(mount.id, s.name)} name={<code>{s.name}</code>} sub={`version ${s.current_version}`} />,
              },
              {
                key: "state",
                label: "State",
                sort: (s) => (s.destroyed_at ? 2 : s.deleted ? 1 : 0),
                render: (s) => {
                  const st = versionState(s);
                  return <Status value={st === "active" ? "live" : st} tone={STATE_TONE[st]} />;
                },
              },
              { key: "version", label: "Version", numeric: true, sort: (s) => Number(s.current_version), render: (s) => <span className="font-mono">{s.current_version}</span> },
              { key: "updated", label: "Updated", sort: (s) => s.updated_at, render: (s) => <When value={s.updated_at} /> },
              { key: "created", label: "Created", sort: (s) => s.created_at, render: (s) => <When value={s.created_at} /> },
            ]}
          />
          {paged && (
            <div className="mt-3 flex items-center justify-end gap-2">
              <Button disabled={cursors.length < 2} onClick={() => setCursors((c) => c.slice(0, -1))}>
                <ChevronLeft size={14} aria-hidden /> Previous
              </Button>
              <Button disabled={!secrets.data?.next_cursor} onClick={() => setCursors((c) => [...c, secrets.data!.next_cursor!])}>
                Next <ChevronRight size={14} aria-hidden />
              </Button>
            </div>
          )}
        </div>
      </Split>
      {writing && <WriteSecretDrawer mountId={mount.id} existing={existing} onClose={() => setWriting(false)} onWritten={(w) => navigate(secretPath(mount.id, w.name))} />}
    </Page>
  );
}

/* ------------------------------------------------------------------ one secret */

type Revealed = { version: number; value: Record<string, unknown> };

function SecretView({ mount, name }: { mount: Row; name: string }) {
  const toast = useToast();
  const metadata = useRead<Row>("secret_metadata", { mount_id: mount.id, name });
  const [writing, setWriting] = useState(false);
  const [revealed, setRevealed] = useState<Revealed | null>(null);
  const [revealing, setRevealing] = useState<number | null>(null);
  const [revealError, setRevealError] = useState<unknown>(null);
  const back = (
    <Button to={`/secrets?mount_id=${encodeURIComponent(mount.id)}`}>
      <ArrowLeft size={14} aria-hidden /> {mount.name}
    </Button>
  );
  if (metadata.isError) {
    return (
      <Page eyebrow="Organization" title={<span className="font-mono">{name}</span>} actions={back}>
        <ErrorBlock error={metadata.error} retry={() => void metadata.refetch()} what="the secret's metadata" />
      </Page>
    );
  }
  const m = metadata.data;
  if (!m) {
    return (
      <Page eyebrow="Organization" title={<span className="font-mono">{name}</span>} actions={back}>
        <Skeleton lines={5} />
      </Page>
    );
  }
  const versions = (m.versions ?? []) as Row[];
  const current = versions.find((v) => Number(v.version) === Number(m.current_version));
  const currentState = current ? versionState(current) : null;
  const count = (st: VersionState) => versions.filter((v) => versionState(v) === st).length;

  /** Reveal is a deliberate read of one version; its value stays in this component only. */
  const reveal = async (version: number) => {
    setRevealing(version);
    setRevealError(null);
    try {
      const got = await read<Row>("read_secret", { mount_id: mount.id, name, version });
      setRevealed({ version: Number(got.version), value: (got.value ?? {}) as Record<string, unknown> });
    } catch (e) {
      setRevealed(null);
      setRevealError(e);
    } finally {
      setRevealing(null);
    }
  };
  const change = async (capability: "delete_secret_versions" | "undelete_secret_versions" | "destroy_secret_versions", version: number, done: string) => {
    await act(capability, { mount_id: mount.id, name, versions: [version] });
    if (revealed?.version === version && capability !== "undelete_secret_versions") setRevealed(null);
    toast(done);
  };
  const revealCurrent = currentState === "active" && (
    <Button onClick={() => (revealed?.version === Number(m.current_version) ? setRevealed(null) : void reveal(Number(m.current_version)))} loading={revealing === Number(m.current_version)}>
      {revealed?.version === Number(m.current_version) ? <EyeOff size={14} aria-hidden /> : <Eye size={14} aria-hidden />}{" "}
      {revealed?.version === Number(m.current_version) ? "Hide value" : "Reveal value"}
    </Button>
  );

  return (
    <Page
      eyebrow="Organization"
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          <span className="font-mono">{name}</span>
          {currentState && <Status value={currentState === "active" ? "live" : currentState} tone={STATE_TONE[currentState]} />}
        </span>
      }
      sub={
        <>
          In <Link to={`/secrets?mount_id=${encodeURIComponent(mount.id)}`}>{mount.name}</Link>. Every write is a new version; values are shown only when you reveal one.
        </>
      }
      actions={
        <>
          {revealCurrent}
          <Button variant="primary" onClick={() => setWriting(true)}>
            <Plus size={14} aria-hidden /> New version
          </Button>
        </>
      }
    >
      <StatRow cols={4}>
        <Stat label="Current version" value={m.current_version ?? "—"} icon={History} />
        <Stat label="Live" value={count("active")} sub={plural(versions.length, "version") + " in all"} tone="ok" />
        <Stat label="Deleted" value={count("deleted")} sub="can be undeleted" />
        <Stat label="Destroyed" value={count("destroyed")} sub="their data is gone" tone={count("destroyed") ? "bad" : undefined} />
      </StatRow>
      <Split
        side={
          <>
            <Card label="Secret" title={<code>{name}</code>}>
              <KeyValue
                rows={[
                  ["Current version", <span className="font-mono">{m.current_version}</span>],
                  ["Created", <When value={m.created_at} />],
                  ["Updated", <When value={m.updated_at} />],
                  [
                    "Custom metadata",
                    Object.keys(m.custom_metadata ?? {}).length ? <code className="break-all">{JSON.stringify(m.custom_metadata)}</code> : <span className="muted">none</span>,
                  ],
                ]}
              />
            </Card>
            <MountFacts mount={mount} />
          </>
        }
      >
        {revealError ? <ErrorBlock error={revealError} what="the value" /> : null}
        {revealed && (
          <Card
            label={`Version ${revealed.version}`}
            title="Value"
            tone="active"
            actions={
              <Button variant="ghost" onClick={() => setRevealed(null)}>
                <EyeOff size={14} aria-hidden /> Hide
              </Button>
            }
          >
            <RevealedValue value={revealed.value} />
          </Card>
        )}
        {currentState && currentState !== "active" && (
          <Banner tone="quiet" title={`The current version is ${currentState}`}>
            Reading this secret fails until a version is {currentState === "deleted" ? "undeleted or " : ""}written again.
          </Banner>
        )}
        <Card label="Version history" padded={false}>
          <DataTable
            rows={versions}
            rowKey={(v) => String(v.version)}
            empty={
              <div className="p-4">
                <PanelEmpty>No versions.</PanelEmpty>
              </div>
            }
            initialSort={{ key: "version", dir: "descending" }}
            columns={[
              {
                key: "version",
                label: "Version",
                numeric: true,
                sort: (v) => Number(v.version),
                render: (v) => (
                  <span className="font-mono">
                    {v.version}
                    {Number(v.version) === Number(m.current_version) && <span className="ml-1.5 text-[11px] text-ink-3">current</span>}
                  </span>
                ),
              },
              { key: "state", label: "State", render: (v) => <Status value={versionState(v) === "active" ? "live" : versionState(v)} tone={STATE_TONE[versionState(v)]} /> },
              { key: "created", label: "Created", render: (v) => <When value={v.created_at} /> },
              { key: "deleted", label: "Deleted", render: (v) => <When value={v.deleted_at} /> },
              {
                key: "actions",
                label: <span className="sr-only">Actions</span>,
                render: (v) => <VersionActions version={v} name={name} revealed={revealed?.version === Number(v.version)} revealing={revealing === Number(v.version)} onReveal={reveal} onHide={() => setRevealed(null)} onChange={change} onFailed={(message) => toast(message, "bad")} />,
              },
            ]}
          />
        </Card>
      </Split>
      {writing && (
        <WriteSecretDrawer
          mountId={mount.id}
          name={name}
          currentVersion={Number(m.current_version) || undefined}
          onClose={() => setWriting(false)}
          onWritten={() => setRevealed(null)}
        />
      )}
    </Page>
  );
}

function VersionActions({
  version: v,
  name,
  revealed,
  revealing,
  onReveal,
  onHide,
  onChange,
  onFailed,
}: {
  version: Row;
  name: string;
  revealed: boolean;
  revealing: boolean;
  onReveal: (version: number) => void;
  onHide: () => void;
  onChange: (capability: "delete_secret_versions" | "undelete_secret_versions" | "destroy_secret_versions", version: number, done: string) => Promise<void>;
  onFailed: (message: string) => void;
}) {
  const n = Number(v.version);
  const st = versionState(v);
  if (st === "destroyed") return <span className="muted text-[12.5px]">erased</span>;
  return (
    <span className="flex flex-wrap justify-end gap-1.5">
      {st === "active" && (
        <Button variant="ghost" onClick={() => (revealed ? onHide() : onReveal(n))} loading={revealing} aria-label={revealed ? `Hide version ${n}` : `Reveal version ${n}`}>
          {revealed ? <EyeOff size={14} aria-hidden /> : <FileKey size={14} aria-hidden />} {revealed ? "Hide" : "Reveal"}
        </Button>
      )}
      {st === "active" ? (
        <ConfirmButton
          variant="ghost"
          title={`Delete Version ${n}?`}
          confirmLabel="Delete version"
          confirm={
            <>
              Version {n} of <code>{name}</code> becomes unreadable. Its data is kept, and you can undelete it later.
            </>
          }
          onConfirm={() => onChange("delete_secret_versions", n, `Deleted version ${n}`)}
        >
          <Trash2 size={14} aria-hidden /> Delete
        </ConfirmButton>
      ) : (
        <Button variant="ghost" onClick={() => onChange("undelete_secret_versions", n, `Undeleted version ${n}`).catch((e: Error) => onFailed(e.message))}>
          <Undo2 size={14} aria-hidden /> Undelete
        </Button>
      )}
      <ConfirmButton
        variant="ghost"
        danger
        title={`Destroy Version ${n}?`}
        confirmLabel="Destroy for good"
        typed={name}
        confirm={
          <>
            The value of version {n} of <code>{name}</code> is erased forever. It can never be read or undeleted again; only its history row stays.
          </>
        }
        onConfirm={() => onChange("destroy_secret_versions", n, `Destroyed version ${n}`)}
      >
        <Eraser size={14} aria-hidden /> Destroy
      </ConfirmButton>
    </span>
  );
}
