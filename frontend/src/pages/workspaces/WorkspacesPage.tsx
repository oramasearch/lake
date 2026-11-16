// SPDX-License-Identifier: MIT

/** Ship → Workspaces: every workspace the person can reach, grouped by owner as the top bar's
 * picker groups them (lake#638), with their phase and storage, New Workspace in a dialog that asks
 * the owner, Rename, Settings (the model the workspace's agents call, lake#1230), and the typed-name
 * deletion (`?delete=<id>`, a dialog) that first lists what
 * goes with counts from `delete_workspace {dry_run: true}`. Where Lake runs in regions, each
 * workspace shows its region: the region of this Lake, where it was created (lake#757). Not a
 * workspace page: it is never under
 * `/w/<id>/`, so every call here goes unprefixed with the customer named. */
import { type ReactNode, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQueries, useQuery } from "@tanstack/react-query";
import { Boxes, HardDrive, Pencil, Plus, SearchX, Settings2, Trash2 } from "lucide-react";
import { act, read, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { initialOf, orderOwners, ownerKind, ownerName, phaseOf, workspaceFigures, workspaceGroups } from "@/lib/workspacePicker";
import { Button, type Column, cx, DataTable, Empty, ErrorBlock, Field, FilterChips, Form, Page, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar, fieldError } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { postRead } from "@/pages/design/api";
import { FormError } from "@/pages/design/parts";
import { OrderSheetLinks, useServerRegions } from "@/components/Region";
import { agentModelInput, agentModelLabel, agentModelPrice, currentAgentModel, defaultAgentModel, longPromptPrice, type WorkspaceSettings } from "@/lib/agentModels";
import { workspaceRegionLabel } from "@/lib/regions";
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

export default function WorkspacesPage() {
  const { viewer, space, refresh } = useSession();
  const [params, setParams] = useSearchParams();
  // Every workspace the person can reach, grouped by owner as the picker groups them (lake#638).
  // `?customer_id=` only says which owner a new workspace defaults to.
  const asked = params.get("customer_id");
  const customerId = viewer.spaces.find((s) => s.id === asked)?.id ?? space?.id ?? viewer.spaces.find((s) => s.personal)?.id ?? viewer.spaces[0]?.id ?? "";
  const doomed = params.get("delete") ?? "";
  const back = `/workspaces?customer_id=${encodeURIComponent(customerId)}`;
  // `?new=1` (the top bar's "New workspace") opens the dialog for that owner; closing drops it.
  const [creating, setCreatingState] = useState<string | null>(params.get("new") === "1" ? customerId : null);
  const setCreating = (owner: string | null) => {
    setCreatingState(owner);
    if (!owner && params.has("new")) {
      const next = new URLSearchParams(params);
      next.delete("new");
      setParams(next, { replace: true });
    }
  };
  const [renaming, setRenaming] = useState<{ owner: string; row: Workspace } | null>(null);
  const [configuring, setConfiguring] = useState<{ owner: string; row: Workspace } | null>(null);
  const [search, setSearch] = useState("");
  const [phase, setPhase] = useState<string | null>(null);
  // The region every workspace here belongs to; null on a Lake without regions (lake#757).
  const region = workspaceRegionLabel(useServerRegions());

  useTrail([{ label: "Ship", to: "/ship/models" }, { label: "Workspaces", to: back }], "Workspaces");

  const owners = orderOwners(viewer.spaces);
  const lists = useQueries({
    queries: owners.map((s) => ({
      queryKey: ["list_workspaces", "", { customer_id: s.id }],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<{ workspaces?: Workspace[] }>("list_workspaces", { customer_id: s.id }, { workspace: null, signal }),
    })),
  });
  const listed: Record<string, Workspace[] | undefined> = {};
  owners.forEach((s, i) => {
    listed[s.id] = lists[i]?.data ? (lists[i].data?.workspaces ?? []) : undefined;
  });
  const failedList = lists.find((l) => l.isError);
  const loading = lists.some((l) => l.isLoading);
  const groups = workspaceGroups(owners, listed);
  const rows = groups.flatMap((g) => g.rows ?? []);
  const figures = workspaceFigures(rows);
  const phases = new Map<string, number>();
  for (const r of rows) phases.set(phaseOf(r), (phases.get(phaseOf(r)) ?? 0) + 1);
  const needle = search.trim().toLowerCase();
  const matches = (r: Workspace) => (!phase || phaseOf(r) === phase) && (!needle || `${r.name} ${r.environment ?? ""} ${r.namespace ?? ""}`.toLowerCase().includes(needle));
  const filtering = !!needle || !!phase;
  const ownerOf = (id: string) => groups.find((g) => g.rows?.some((r) => r.id === id))?.space.id ?? customerId;
  const askDelete = (id: string) => {
    const next = new URLSearchParams(params);
    next.set("customer_id", ownerOf(id));
    next.set("delete", id);
    setParams(next);
  };
  const columns = (owner: string): Column<Workspace>[] => [
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
                      {region ? <span data-workspace-region>{` · ${region}`}</span> : null}
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
                    <span className="inline-flex flex-wrap justify-end gap-1">
                      <Button variant="ghost" onClick={() => setRenaming({ owner, row })} aria-label={`Rename ${row.name}`}>
                        <Pencil size={14} aria-hidden /> Rename
                      </Button>
                      <Button variant="ghost" onClick={() => setConfiguring({ owner, row })} aria-label={`Settings of ${row.name}`}>
                        <Settings2 size={14} aria-hidden /> Settings
                      </Button>
                      {/* The confirmation lists what the deletion destroys before it asks for the name. */}
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
                    </span>
                  ) : (
                    <span className="text-[12.5px] text-ink-3">Deletion requested</span>
                  ),
              },
            ];

  return (
    <Page
      eyebrow="Ship"
      title="Workspaces"
      sub={`Isolated environments, each keeping its own projects, data and models apart from every other.${region ? ` Workspaces here are in ${region}.` : ""}`}
      actions={
        customerId ? (
          <>
            <OrderSheetLinks />
            <Button variant="primary" onClick={() => setCreating(customerId)}>
              <Plus size={14} aria-hidden /> New workspace
            </Button>
          </>
        ) : (
          <OrderSheetLinks />
        )
      }
    >
      {!customerId ? (
        <Empty icon={Boxes} title="No Workspace Yet">
          You have no workspace yet.
        </Empty>
      ) : loading ? (
        <Skeleton />
      ) : failedList ? (
        <ErrorBlock error={failedList.error} retry={() => lists.forEach((l) => void l.refetch())} what="workspaces" />
      ) : (
        <>
          <StatRow cols={4}>
            <Stat label="Workspaces" value={figures.workspaces} icon={Boxes} sub={owners.length > 1 ? `across ${plural(owners.length, "owner", "owners")}` : undefined} />
            <Stat label="Ready" value={figures.ready} tone={figures.ready ? "ok" : undefined} sub={`${figures.workspaces - figures.ready - figures.failed} not ready yet`} />
            <Stat label="Failed" value={figures.failed} tone={figures.failed ? "bad" : undefined} />
            <Stat label="Storage" value={`${figures.storage} GiB`} icon={HardDrive} sub="requested across workspaces" />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search workspaces…" label="Search workspaces" />
            <FilterChips options={[...phases].map(([id, count]) => ({ id, label: id.charAt(0).toUpperCase() + id.slice(1), count }))} value={phase} onChange={setPhase} />
          </Toolbar>
          <div className="flex flex-col gap-6">
            {groups.map(({ space: owner, rows: ownRows }) => {
              const shown = (ownRows ?? []).filter(matches);
              if (filtering && !shown.length) return null;
              return (
                <section key={owner.id} aria-label={`${ownerKind(owner)}: ${ownerName(owner, viewer)}`} className="flex flex-col gap-2">
                  <div className="flex items-center gap-2">
                    <OwnerInitial space={owner} name={ownerName(owner, viewer)} />
                    <div className="min-w-0 flex-1 leading-tight">
                      <div className="text-[11.5px] text-ink-3">{ownerKind(owner)}</div>
                      {/* The owner's name as written: not an h2, whose page style is an upper-cased label. */}
                      <div role="heading" aria-level={2} className="truncate text-[15px] font-semibold text-ink">
                        {ownerName(owner, viewer)}
                      </div>
                    </div>
                    <span className="font-mono text-[12px] text-ink-3">{plural((ownRows ?? []).length, "workspace", "workspaces")}</span>
                  </div>
                  {ownRows && ownRows.length ? (
                    <DataTable<Workspace> rows={shown} what="workspaces" caption={`Workspaces of ${ownerName(owner, viewer)}`} rowKey={(row) => row.id} columns={columns(owner.id)} />
                  ) : (
                    <button
                      type="button"
                      onClick={() => setCreating(owner.id)}
                      className="flex w-full items-center gap-2 rounded-[6px] border border-dashed border-rule px-3 py-3 text-left text-[13px] text-ink-2 hover:border-action hover:text-ink"
                    >
                      <Plus size={14} aria-hidden /> New workspace
                      <span className="text-ink-3">· {ownerName(owner, viewer)} has none yet</span>
                    </button>
                  )}
                </section>
              );
            })}
            {filtering && !rows.some(matches) && (
              <Empty compact icon={SearchX} title="No Workspace Matches">
                Clear the search or the filter to see more.
              </Empty>
            )}
          </div>
          {doomed && <DeleteConfirmation key={doomed} customerId={ownerOf(doomed)} workspaceId={doomed} back={back} onDone={refresh} />}
          {renaming && <RenameWorkspace customerId={renaming.owner} workspace={renaming.row} onDone={refresh} onClose={() => setRenaming(null)} />}
          {configuring && <WorkspaceSettingsDialog customerId={configuring.owner} workspace={configuring.row} onClose={() => setConfiguring(null)} />}
          {creating && (
            <NewWorkspace
              customerId={creating}
              onDone={(owner) => {
                refresh();
                if (owner !== customerId) {
                  const next = new URLSearchParams(params);
                  next.delete("new");
                  next.set("customer_id", owner);
                  setParams(next, { replace: true });
                }
              }}
              onClose={() => setCreating(null)}
            />
          )}
        </>
      )}
    </Page>
  );
}

/** An owner's initial, as the top bar's picker draws it. */
function OwnerInitial({ space, name }: { space: { personal: boolean }; name: string }) {
  return (
    <span
      aria-hidden
      className={cx(
        "inline-flex h-7 w-7 shrink-0 items-center justify-center rounded-[6px] text-[12.5px] font-semibold",
        space.personal ? "bg-ai-soft text-ai" : "bg-action-soft text-action",
      )}
    >
      {initialOf(name)}
    </span>
  );
}

/** Renames a workspace (`update_workspace {name}`); an account's first workspace created as
 * `workspace` is renamed here, never by lake (lake#638). */
function RenameWorkspace({ customerId, workspace, onDone, onClose }: { customerId: string; workspace: Workspace; onDone: () => void; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState(workspace.name);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("update_workspace", { customer_id: customerId, workspace_id: workspace.id, name: name.trim() }, { workspace: null });
      onDone();
      toast("Workspace renamed");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title="Rename Workspace"
      subtitle="Only its name changes: its data, address and namespace stay."
      onClose={onClose}
      width={440}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="rename-workspace" variant="primary" loading={busy} disabled={!name.trim() || name.trim() === workspace.name}>
            Rename
          </Button>
        </>
      }
    >
      <Form id="rename-workspace" onSubmit={submit}>
        <FormError error={error && !fieldError(error, "name") ? error : null} />
        <Field label="Name" required error={fieldError(error, "name")}>
          <input name="name" maxLength={128} value={name} autoFocus onChange={(e) => setName(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

/** A workspace's settings: the model its agents call through Lake (lake#1230), one of the chat
 * models this Lake serves, each with its price per million tokens from the price list in force.
 * Unset means the default, gpt-6.1-sol; choosing the default sends null. Those who may only read
 * the workspace see the choice and cannot save it (the save answers why). */
function WorkspaceSettingsDialog({ customerId, workspace, onClose }: { customerId: string; workspace: Workspace; onClose: () => void }) {
  const toast = useToast();
  const settings = useQuery<WorkspaceSettings>({
    queryKey: ["get_workspace_settings", customerId, workspace.id],
    queryFn: ({ signal }) => read<WorkspaceSettings>("get_workspace_settings", { customer_id: customerId, workspace_id: workspace.id }, { workspace: null, signal }),
  });
  const [choice, setChoice] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const current = currentAgentModel(settings.data);
  const selected = choice ?? current;
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("set_workspace_settings", { customer_id: customerId, workspace_id: workspace.id, ...agentModelInput(selected, settings.data) }, { workspace: null });
      void settings.refetch();
      toast(`Agents in ${workspace.name} now use ${selected}`);
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const models = settings.data?.agent_models ?? [];
  let body;
  if (settings.isPending) body = <Skeleton />;
  else if (settings.isError) body = <ErrorBlock error={settings.error} retry={() => void settings.refetch()} what="this workspace's settings" />;
  else
    body = (
      <Form id="workspace-settings" onSubmit={submit}>
        <FormError error={error && !fieldError(error, "agent_model") ? error : null} />
        <fieldset className="m-0 border-0 p-0">
          <legend className="label mb-2">Agent model</legend>
          <p className="mt-0 mb-2 text-[12.5px] text-ink-2">Every agent working in this workspace uses this model, and its tokens are billed at its price.</p>
          <div className="grid gap-2" data-agent-models>
            {models.map((option) => {
              const long = longPromptPrice(option, settings.data?.currency);
              return (
                <label
                  key={option.model}
                  className={cx("flex cursor-pointer gap-3 rounded-[6px] border p-3", selected === option.model ? "border-action bg-action-soft" : "border-hairline bg-surface hover:border-rule")}
                >
                  <input type="radio" name="agent_model" value={option.model} checked={selected === option.model} onChange={() => setChoice(option.model)} className="mt-1" />
                  <span className="min-w-0">
                    <span className="block font-mono text-[13px] font-semibold text-ink">{agentModelLabel(option, defaultAgentModel(settings.data))}</span>
                    <span className="block text-[12.5px] text-ink-2">{agentModelPrice(option, settings.data?.currency)}</span>
                    {long && <span className="block text-[12px] text-ink-3">{long}</span>}
                  </span>
                </label>
              );
            })}
          </div>
          {fieldError(error, "agent_model") && <p className="mt-2 text-[12.5px] text-bad">{fieldError(error, "agent_model")}</p>}
        </fieldset>
      </Form>
    );
  return (
    <Modal
      title={`${workspace.name} Settings`}
      subtitle="Choose the model this workspace's agents use."
      onClose={onClose}
      width={520}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="workspace-settings" variant="primary" loading={busy} disabled={!settings.data || selected === current}>
            Save
          </Button>
        </>
      }
    >
      {body}
    </Modal>
  );
}

function NewWorkspace({ customerId, onDone, onClose }: { customerId: string; onDone: (owner: string) => void; onClose: () => void }) {
  const toast = useToast();
  const { viewer } = useSession();
  // Which account or organization owns it: asked only when the viewer has more than one (lake#638).
  const [owner, setOwner] = useState(customerId);
  const [name, setName] = useState("");
  const [environment, setEnvironment] = useState("dev");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("create_workspace", { customer_id: owner, name, environment }, { workspace: null });
      onDone(owner);
      toast("Workspace created");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const generic = error && !fieldError(error, "name") && !fieldError(error, "environment") ? error : null;
  const region = workspaceRegionLabel(useServerRegions());
  return (
    <Modal
      title="New Workspace"
      subtitle={
        region
          ? `Lake prepares it in ${region}, where it stays; preparing it takes a minute or two.`
          : "Lake prepares it for you; preparing it takes a minute or two."
      }
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
        {viewer.spaces.length > 1 && (
          <Field label="Owner" required help="Your account, or an organization you belong to.">
            <select name="owner" value={owner} onChange={(e) => setOwner(e.target.value)}>
              {viewer.spaces.map((s) => (
                <option key={s.id} value={s.id}>
                  {ownerKind(s)}: {ownerName(s, viewer)}
                </option>
              ))}
            </select>
          </Field>
        )}
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
          {item(null, "Everything it holds: every record, dataset, topic and store, and its own audit history.")}
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
