/** Ship → Workspaces (lake's workspaces_content and workspace_deletion_confirmation): a customer's
 * workspaces with their phase and storage, New Workspace in a dialog, and the typed-name deletion
 * (`?delete=<id>`, a dialog) that first lists what goes with counts from
 * `delete_workspace {dry_run: true}`. Not a workspace page: it is never under `/w/<id>/`, so every
 * call here goes unprefixed with the customer named. */
import { type ReactNode, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { Boxes, HardDrive, Plus, SearchX, Trash2 } from "lucide-react";
import { act, useRead, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, DataTable, Empty, ErrorBlock, Field, FilterChips, Form, Page, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar, fieldError } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { postRead } from "@/pages/design/api";
import { FormError } from "@/pages/design/parts";
import "@/styles/design.css";

type Workspace = {
  id: string;
  name: string;
  environment?: string;
  phase?: string;
  namespace?: string;
  created_at?: string;
  desired?: string;
  storage_gi?: number;
  storage_gi_observed?: number;
  storage_phase?: string;
};
type Plan = {
  workspace_id: string;
  workspace_name: string;
  counts?: Record<string, number>;
  repositories?: { name?: string; state?: string }[];
  apps?: { name?: string }[];
  images?: { name?: string; tags?: unknown[] }[];
};

const plural = (n: number, one: string, many: string) => `${n} ${n === 1 ? one : many}`;

/** The phase a row shows: a requested deletion first, then the operator's phase. */
function phaseOf(row: Workspace): string {
  return row.desired === "deleted" ? "deleting" : row.phase || "pending";
}

export default function WorkspacesPage() {
  const { viewer, space, refresh } = useSession();
  const [params, setParams] = useSearchParams();
  // The customer: an accessible `?customer_id=`, else the current workspace's, else the first.
  const asked = params.get("customer_id");
  const customerId = viewer.spaces.find((s) => s.id === asked)?.id ?? space?.id ?? viewer.customer_id ?? viewer.spaces[0]?.id ?? "";
  const doomed = params.get("delete") ?? "";
  const back = `/workspaces?customer_id=${encodeURIComponent(customerId)}`;
  const [creating, setCreating] = useState(false);
  const [search, setSearch] = useState("");
  const [phase, setPhase] = useState<string | null>(null);

  useTrail([{ label: "Ship", to: "/ship/models" }, { label: "Workspaces", to: back }], "Workspaces");

  const listed = useRead<{ workspaces?: Workspace[] }>("list_workspaces", { customer_id: customerId }, { workspace: null, enabled: !!customerId });
  const rows = listed.data?.workspaces ?? [];
  const phases = new Map<string, number>();
  for (const r of rows) phases.set(phaseOf(r), (phases.get(phaseOf(r)) ?? 0) + 1);
  const needle = search.trim().toLowerCase();
  const shown = rows.filter((r) => (!phase || phaseOf(r) === phase) && (!needle || `${r.name} ${r.environment ?? ""} ${r.namespace ?? ""}`.toLowerCase().includes(needle)));
  const ready = rows.filter((r) => phaseOf(r) === "Ready").length;
  const failed = rows.filter((r) => phaseOf(r) === "Failed").length;
  const storage = rows.reduce((sum, r) => sum + (typeof r.storage_gi === "number" ? r.storage_gi : 0), 0);
  const askDelete = (id: string) => {
    const next = new URLSearchParams(params);
    next.set("customer_id", customerId);
    next.set("delete", id);
    setParams(next);
  };

  return (
    <Page
      eyebrow="Ship"
      title="Workspaces"
      sub="Isolated environments, each with a Zetta of its own for its projects, data and models."
      actions={
        customerId ? (
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New workspace
          </Button>
        ) : undefined
      }
    >
      {!customerId ? (
        <Empty icon={Boxes} title="No Workspace Yet">
          You have no workspace yet.
        </Empty>
      ) : listed.isLoading ? (
        <Skeleton />
      ) : listed.isError ? (
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="workspaces" />
      ) : (
        <>
          <StatRow cols={4}>
            <Stat label="Workspaces" value={rows.length} icon={Boxes} />
            <Stat label="Ready" value={ready} tone={ready ? "ok" : undefined} sub={`${rows.length - ready - failed} not ready yet`} />
            <Stat label="Failed" value={failed} tone={failed ? "bad" : undefined} />
            <Stat label="Storage" value={`${storage} GiB`} icon={HardDrive} sub="requested across workspaces" />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search workspaces…" label="Search workspaces" />
            <FilterChips options={[...phases].map(([id, count]) => ({ id, label: id.charAt(0).toUpperCase() + id.slice(1), count }))} value={phase} onChange={setPhase} />
          </Toolbar>
          <DataTable<Workspace>
            rows={shown}
            q={listed}
            what="workspaces"
            caption="Workspaces"
            rowKey={(row) => row.id}
            initialSort={{ key: "name", dir: "ascending" }}
            empty={
              rows.length ? (
                <Empty compact icon={SearchX} title="No Workspace Matches">
                  Clear the search or the filter to see more.
                </Empty>
              ) : (
                <Empty
                  icon={Boxes}
                  title="No Workspaces Yet"
                  action={
                    <Button variant="primary" onClick={() => setCreating(true)}>
                      <Plus size={14} aria-hidden /> New workspace
                    </Button>
                  }
                >
                  A workspace gets its own Zetta, where its projects, data and models live.
                </Empty>
              )
            }
            columns={[
              {
                key: "name",
                label: "Workspace",
                sort: (row) => row.name.toLowerCase(),
                render: (row) => (
                  <div className="min-w-0">
                    <Link to={`/w/${encodeURIComponent(row.id)}/design/projects`} className="font-semibold">
                      {row.name}
                    </Link>
                    <div className="text-[12px] text-ink-3">
                      {row.environment || "—"}
                      {row.namespace ? (
                        <>
                          {" · "}
                          <span className="font-mono">{row.namespace}</span>
                        </>
                      ) : null}
                    </div>
                  </div>
                ),
              },
              { key: "phase", label: "Phase", sort: (row) => phaseOf(row), render: (row) => <Status value={phaseOf(row)} /> },
              {
                key: "storage",
                label: "Storage",
                numeric: true,
                sort: (row) => row.storage_gi ?? null,
                render: (row) =>
                  typeof row.storage_gi === "number" && row.storage_gi > 0 ? (
                    <div className="flex flex-col items-end gap-0.5">
                      <span className="font-mono">
                        {typeof row.storage_gi_observed === "number" && row.storage_gi_observed > 0 && row.storage_gi_observed !== row.storage_gi
                          ? `${row.storage_gi_observed} / ${row.storage_gi} GiB`
                          : `${row.storage_gi} GiB`}
                      </span>
                      {row.storage_phase && row.storage_phase !== "Ready" && <Status value={row.storage_phase} />}
                    </div>
                  ) : (
                    <span className="text-ink-3">—</span>
                  ),
              },
              {
                key: "created",
                label: "Created",
                sort: (row) => row.created_at ?? "",
                render: (row) => (row.created_at ? <span title={dateTime(row.created_at)}>{relative(row.created_at)}</span> : ""),
              },
              {
                key: "actions",
                label: "",
                className: "text-right",
                render: (row) =>
                  row.desired !== "deleted" ? (
                    // The confirmation lists what the deletion destroys before it asks for the name.
                    <a
                      href={`${back}&delete=${encodeURIComponent(row.id)}#delete-workspace`}
                      className="button button-ghost"
                      onClick={(e) => {
                        e.preventDefault();
                        askDelete(row.id);
                      }}
                    >
                      <Trash2 size={14} aria-hidden /> Delete
                    </a>
                  ) : (
                    <span className="text-[12.5px] text-ink-3">Deletion requested</span>
                  ),
              },
            ]}
          />
          {doomed && <DeleteConfirmation key={doomed} customerId={customerId} workspaceId={doomed} back={back} onDone={refresh} />}
          {creating && <NewWorkspace customerId={customerId} onDone={refresh} onClose={() => setCreating(false)} />}
        </>
      )}
    </Page>
  );
}

function NewWorkspace({ customerId, onDone, onClose }: { customerId: string; onDone: () => void; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [environment, setEnvironment] = useState("dev");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("create_workspace", { customer_id: customerId, name, environment }, { workspace: null });
      onDone();
      toast("Workspace created");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const generic = error && !fieldError(error, "name") && !fieldError(error, "environment") ? error : null;
  return (
    <Modal
      title="New Workspace"
      subtitle="It gets a Zetta of its own; preparing it takes a minute or two."
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-workspace" variant="primary" loading={busy}>
            New workspace
          </Button>
        </>
      }
    >
      <Form id="new-workspace" onSubmit={submit}>
        <FormError error={generic} />
        <Field label="Name" required error={fieldError(error, "name")}>
          <input name="name" maxLength={128} value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Environment" required help="Use dev, staging, prod or a lowercase DNS label." error={fieldError(error, "environment")}>
          <input name="environment" maxLength={47} pattern="[a-z0-9]([a-z0-9-]*[a-z0-9])?" value={environment} onChange={(e) => setEnvironment(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

function DeleteConfirmation({ customerId, workspaceId, back, onDone }: { customerId: string; workspaceId: string; back: string; onDone: () => void }) {
  const navigate = useNavigate();
  const toast = useToast();
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  // The dry run deletes nothing: a read over POST, so it refreshes nothing on screen. It is not
  // asked again while the deletion itself is under way.
  const plan = useQuery<Plan>({
    queryKey: ["delete_workspace", "dry_run", customerId, workspaceId],
    queryFn: ({ signal }) => postRead<Plan>("delete_workspace", { customer_id: customerId, workspace_id: workspaceId, dry_run: true }, { signal, workspace: null }),
    retry: false,
    enabled: !busy,
  });
  const close = () => navigate(back);
  const value = plan.data;
  const name = value?.workspace_name ?? "";

  const submit = async () => {
    if (!value) return;
    setBusy(true);
    setError(null);
    try {
      await act("delete_workspace", { customer_id: customerId, workspace_id: value.workspace_id, confirm } as Row, { workspace: null });
      onDone();
      toast(`Deleting ${name}`);
      navigate(back);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  let body;
  if (plan.isPending && !plan.data) body = plan.fetchStatus === "idle" ? null : <Skeleton />;
  else if (plan.isError) body = <ErrorBlock error={plan.error} what="what this deletion destroys" />;
  else if (!value || !value.workspace_id) return null;
  else {
    const counts = value.counts ?? {};
    const count = (key: string) => (typeof counts[key] === "number" ? counts[key] : 0);
    const item = (figure: number | null, children: ReactNode) => (
      <li className="flex items-start gap-3 py-2">
        <span className="w-10 shrink-0 text-right font-mono text-[15px] font-semibold tabular-nums text-bad">{figure ?? "·"}</span>
        <span className="min-w-0 text-[13px]">{children}</span>
      </li>
    );
    body = (
      <>
        <p className="mt-0 text-[13px] text-ink-2">Deleting this workspace destroys everything below. It cannot be undone: retained volume snapshots are the only copy left.</p>
        <ul className="m-0 list-none divide-y divide-hairline rounded-[6px] border border-hairline px-3">
          {item(null, "Its Zetta, with every record, dataset, topic and store in it, and its own audit history.")}
          {item(
            count("repositories"),
            <>
              {plural(count("repositories"), "repository", "repositories")} ({plural(count("live_repositories"), "live", "live")},{" "}
              {plural(count("tombstoned_repositories"), "deleted and still retained", "deleted and still retained")}). Their retention ends now and their names become free.
            </>,
          )}
          {item(count("apps"), `${plural(count("apps"), "app", "apps")}.`)}
          {item(count("builds"), `${plural(count("builds"), "unfinished build", "unfinished builds")}, cancelled.`)}
          {item(count("images"), `${plural(count("images"), "image repository", "image repositories")}, no longer pullable.`)}
        </ul>
        {(!!value.repositories?.length || !!value.apps?.length || !!value.images?.length) && (
          <details className="mt-2">
            <summary className="cursor-pointer text-[12.5px] text-ink-2">Everything by name</summary>
            <ul className="mt-2 space-y-1 pl-4 text-[13px]">
              {(value.repositories ?? []).map((entry, index) => (
                <li key={`r${index}`}>
                  Repository <code>{entry.name ?? ""}</code>
                  {entry.state === "tombstoned" ? " (deleted, retained)" : ""}
                </li>
              ))}
              {(value.apps ?? []).map((entry, index) => (
                <li key={`a${index}`}>
                  App <code>{entry.name ?? ""}</code>
                </li>
              ))}
              {(value.images ?? []).map((entry, index) => {
                const tags = Array.isArray(entry.tags) ? entry.tags.length : 0;
                return (
                  <li key={`i${index}`}>
                    Image <code>{entry.name ?? ""}</code> ({tags} {tags === 1 ? "tag" : "tags"})
                  </li>
                );
              })}
            </ul>
          </details>
        )}
        <Form id="delete-workspace-form" onSubmit={submit} className="mt-3">
          <FormError error={error && !fieldError(error, "confirm") ? error : null} />
          <Field
            label={
              <>
                Type <code>{name}</code> to confirm
              </>
            }
            required
            error={fieldError(error, "confirm")}
          >
            <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
          </Field>
        </Form>
      </>
    );
  }

  return (
    <Modal
      title={<span id="delete-workspace">{name ? `Delete ${name}` : "Delete Workspace"}</span>}
      onClose={close}
      width={560}
      footer={
        <>
          <Button onClick={close}>Cancel</Button>
          <Button type="submit" form="delete-workspace-form" variant="danger" loading={busy} disabled={!value?.workspace_id}>
            <Trash2 size={14} aria-hidden /> Delete workspace and everything in it
          </Button>
        </>
      }
    >
      {body}
    </Modal>
  );
}
