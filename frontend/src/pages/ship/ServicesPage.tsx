/** Services (web_ui.cpp services_content): every database, stream and engine the space runs on
 * lake. Not a workspace page: the services belong to the space. The catalog of kinds is a card
 * grid (each opens the New Service drawer on its kind); the services are a table. A connection is
 * shown in a dialog when asked for, never in the URL, and not kept; deleting one asks for its name. */
import { type ComponentType, useMemo, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import {
  Cable,
  ChartColumn,
  CircleCheck,
  Columns3,
  Database,
  HardDrive,
  Hexagon,
  Layers,
  Loader2,
  Plus,
  Radio,
  Rows3,
  Server,
  Table2,
  Timer,
  Trash2,
  TriangleAlert,
  Zap,
} from "lucide-react";
import { ApiError, act, useRead, type Row } from "@/lib/api";
import { Drawer, Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import {
  Banner,
  Button,
  Card,
  Chip,
  DataTable,
  Empty,
  ErrorBlock,
  Field,
  FilterChips,
  Form,
  Page,
  Section,
  SearchInput,
  Skeleton,
  Stat,
  StatRow,
  Status,
  Toolbar,
  fieldError,
  type Tone,
} from "@/lib/ui";
import { qs, secondsSince, spokenDuration, text, useAsync, wq } from "../train/workspace";
import { CopyMono } from "./parts";

const SUB = "The databases, streams and engines your workspaces run on lake, each its own, with its data kept apart from every other customer's.";

type Kind = { kind: string; label: string; ready_seconds: number };

/** How each kind looks in the catalog: an icon and what it is, in two or three words. */
const KIND_LOOK: Record<string, { icon: ComponentType<{ size?: number; className?: string }>; what: string }> = {
  zetta: { icon: Hexagon, what: "Your workspace engine" },
  postgres: { icon: Database, what: "Relational database" },
  kafka: { icon: Radio, what: "Event streams" },
  clickhouse: { icon: ChartColumn, what: "Columnar analytics" },
  tidb: { icon: Layers, what: "Distributed SQL" },
  cassandra: { icon: Rows3, what: "Wide-column store" },
  valkey: { icon: Zap, what: "Key-value cache" },
  duckdb: { icon: Table2, what: "Analytical SQL" },
  polars: { icon: Columns3, what: "DataFrame engine" },
  sqlite: { icon: HardDrive, what: "Embedded SQL" },
};

function lookOf(kind: string) {
  return KIND_LOOK[kind] ?? { icon: Server, what: "" };
}

/** The status a service shows (service_phase): list_services' `status`, else derived. */
export function servicePhase(service: Row): string {
  const status = text(service, "status");
  if (status) return status;
  const phase = text(service, "phase");
  if (text(service, "desired") === "absent") return !phase || phase === "Ready" ? "Deleting" : phase;
  if (phase === "Ready" && service.connected === false) return "Connecting";
  return phase || "Waiting for the operator";
}

function phaseTone(phase: string): Tone {
  if (phase === "Ready") return "success";
  if (phase === "Failed") return "danger";
  if (phase === "Unassigned") return "neutral";
  return "warning";
}

function settles(phase: string): boolean {
  return phase !== "Ready" && phase !== "Failed" && phase !== "Unassigned";
}

function startHint(service: Row, phase: string, kinds: Kind[]): string {
  if (phase !== "Waiting for the operator" && phase !== "Provisioning" && phase !== "Connecting") return "";
  const kind = kinds.find((k) => k.kind === text(service, "kind"));
  if (!kind) return "";
  const elapsed = secondsSince(text(service, "created_at"));
  let hint = `Usually ready in about ${spokenDuration(kind.ready_seconds)}`;
  if (elapsed >= 0) {
    hint += `; started ${spokenDuration(elapsed)} ago`;
    if (elapsed > 2 * kind.ready_seconds) hint += ". This is taking longer than usual; the message above says what it is waiting for";
  }
  return `${hint}.`;
}

function stallNote(service: Row, phase: string): string {
  let note = text(service, "status_reason");
  if (phase === "Deleting") {
    const waited = secondsSince(text(service, "updated_at"));
    if (waited > 15 * 60) {
      const span = waited >= 2 * 24 * 3600 ? `${Math.floor(waited / 86400)} days` : spokenDuration(waited);
      note += `${note ? " " : ""}Deletion was requested ${span} ago and the operator has not reported it deleted yet.`;
    }
  }
  return note;
}

/** The space this page shows: `?customer_id=` when it is one of the viewer's spaces, else the
 * current one (workspace_selection). */
function useSpaceId(): string {
  const [params] = useSearchParams();
  const { viewer, space } = useSession();
  const asked = params.get("customer_id");
  if (asked && viewer.spaces.some((s) => s.id === asked)) return asked;
  return space?.id ?? viewer.customer_id ?? viewer.spaces[0]?.id ?? "";
}

export default function ServicesPage() {
  const customerId = useSpaceId();
  /** The New Service drawer, open on a kind ("" when closed). */
  const [creating, setCreating] = useState("");
  useTrail([
    { label: "Ship", to: `/ship/models?${qs({ customer_id: customerId })}` },
    { label: "Services", to: `/services?${qs({ customer_id: customerId })}` },
  ]);
  return (
    <Page
      eyebrow="Ship"
      title="Services"
      sub={SUB}
      actions={
        customerId ? (
          <Button variant="primary" onClick={() => setCreating("zetta")}>
            <Plus size={14} aria-hidden /> New service
          </Button>
        ) : null
      }
    >
      {customerId ? (
        <Services customerId={customerId} creating={creating} setCreating={setCreating} />
      ) : (
        <Empty icon={Server} title="No workspace yet" action={<Button to="/workspaces">Workspaces</Button>}>
          You have no workspace yet.
        </Empty>
      )}
    </Page>
  );
}

function Services({ customerId, creating, setCreating }: { customerId: string; creating: string; setCreating: (kind: string) => void }) {
  const { workspaceId, workspaces } = useSession();
  // The current workspace, when it belongs to this space: its connections that use a service.
  const workspace = workspaces.find((w) => w.id === workspaceId && w.customer_id === customerId)?.id ?? "";
  const kindsQ = useRead<{ kinds?: Kind[] }>("list_service_kinds", undefined, { staleTime: 3_600_000 });
  const kinds = useMemo(() => kindsQ.data?.kinds ?? [], [kindsQ.data]);
  const listed = useRead<{ services?: Row[]; limit?: number | string }>(
    "list_services",
    { customer_id: customerId },
    {
      workspace: null,
      refetchInterval: (query) => {
        const services = (query.state.data as { services?: Row[] } | undefined)?.services ?? [];
        return services.some((s) => settles(servicePhase(s))) ? 3_000 : false;
      },
    },
  );
  const connections = useQuery({
    queryKey: ["workspace_query", customerId, workspace, "list_connections", {}],
    queryFn: ({ signal }) => wq<{ connections?: Row[] }>({ customerId, workspaceId: workspace }, "list_connections", {}, signal),
    enabled: !!workspace && listed.isSuccess,
  });
  const [shown, setShown] = useState<Row | null>(null);
  const [deleting, setDeleting] = useState<Row | null>(null);
  const [needle, setNeedle] = useState("");
  const [kindFilter, setKindFilter] = useState<string | null>(null);

  const limit = String(listed.data?.limit ?? "");
  const drawer = creating ? (
    <NewService customerId={customerId} workspaceId={workspace} kinds={kinds} kindsQ={kindsQ} limit={limit} initialKind={creating} onClose={() => setCreating("")} />
  ) : null;
  const catalog = <Catalog kinds={kinds} kindsQ={kindsQ} onPick={setCreating} />;

  if (listed.isLoading) return <Skeleton lines={6} />;
  if (listed.isError) {
    const e = listed.error;
    const unconfigured = e instanceof ApiError && e.code === "store_unavailable" && /not configured/.test(e.message);
    return unconfigured ? (
      <Empty icon={Server} title="Services are not available here yet">
        Services are not available on this lake yet: the operator has not enabled them.
      </Empty>
    ) : e instanceof ApiError && e.status >= 400 && e.status < 500 ? (
      <Empty icon={TriangleAlert} title="Services could not be listed">
        {e.message}
      </Empty>
    ) : (
      <ErrorBlock error={e} retry={() => void listed.refetch()} what="services" />
    );
  }
  const all = listed.data?.services ?? [];
  // Zetta services first (the workspace is one), then the rest in lake's order.
  const services = [...all.filter((s) => text(s, "kind") === "zetta"), ...all.filter((s) => text(s, "kind") !== "zetta")];
  const settling = services.filter((s) => settles(servicePhase(s))).length;
  const ready = services.filter((s) => servicePhase(s) === "Ready").length;
  const failed = services.filter((s) => servicePhase(s) === "Failed").length;
  const used = new Set((connections.data?.connections ?? []).map((c) => text(c, "name")));
  const labelOf = (kind: string) => kinds.find((k) => k.kind === kind)?.label ?? kind;
  const kindCounts = new Map<string, number>();
  for (const s of services) kindCounts.set(text(s, "kind"), (kindCounts.get(text(s, "kind")) ?? 0) + 1);
  const lower = needle.trim().toLowerCase();
  const rows = services.filter(
    (s) =>
      (!lower || [text(s, "name"), text(s, "kind"), labelOf(text(s, "kind")), text(s, "endpoint")].some((v) => v.toLowerCase().includes(lower))) &&
      (!kindFilter || text(s, "kind") === kindFilter),
  );

  return (
    <>
      {services.length === 0 ? (
        <Empty
          icon={Server}
          title="No services yet"
          action={
            <Button variant="primary" onClick={() => setCreating("zetta")}>
              <Plus size={14} aria-hidden /> New service
            </Button>
          }
        >
          Pick a kind from the catalog below to start one.
        </Empty>
      ) : (
        <>
          <StatRow>
            <Stat label="Services" value={services.length} icon={Server} sub={limit && limit !== "0" ? `of ${limit} your workspaces may run at once` : undefined} />
            <Stat label="Ready" value={ready} icon={CircleCheck} tone={ready ? "ok" : undefined} />
            <Stat label="Changing" value={settling} icon={Loader2} sub={settling ? "checked every 3 seconds" : "nothing in progress"} />
            <Stat label="Failed" value={failed} icon={TriangleAlert} tone={failed ? "bad" : undefined} />
          </StatRow>
          <Section title="Your Services">
            <Toolbar>
              <SearchInput value={needle} onChange={setNeedle} placeholder="Search services…" label="Search services" />
              {kindCounts.size > 1 && (
                <FilterChips options={[...kindCounts.entries()].map(([id, count]) => ({ id, label: labelOf(id), count }))} value={kindFilter} onChange={setKindFilter} allLabel="Every kind" />
              )}
            </Toolbar>
            {settling > 0 && <Banner tone="quiet">This page checks again every 3 seconds while a service is changing.</Banner>}
            <DataTable<Row>
              rows={rows}
              caption="Services"
              rowKey={(s) => text(s, "id")}
              empty={<Empty compact title="No service matches">Clear the search or a filter to see more.</Empty>}
              columns={[
                {
                  key: "name",
                  label: "Name",
                  sort: (s) => text(s, "name"),
                  render: (service) => {
                    const kind = text(service, "kind");
                    const Icon = lookOf(kind).icon;
                    return (
                      <div className="flex min-w-0 items-start gap-2">
                        <span className="mt-0.5 flex h-7 w-7 shrink-0 items-center justify-center rounded-[5px] bg-raised text-ink-2">
                          <Icon size={15} aria-hidden />
                        </span>
                        <div className="min-w-0">
                          <div className="flex flex-wrap items-center gap-1.5">
                            <strong className="text-ink">{text(service, "name")}</strong>
                            {service.default === true && <Chip tone="action">workspace</Chip>}
                          </div>
                          <div className="text-[12px] text-ink-3">{labelOf(kind)}</div>
                        </div>
                      </div>
                    );
                  },
                },
                {
                  key: "size",
                  label: "Size",
                  sort: (s) => Number(s.instances ?? 0) * Number(s.storage_gi ?? 0),
                  render: (service) => (
                    <span className="font-mono text-[12.5px] whitespace-nowrap tabular-nums">
                      {text(service, "instances")} × {text(service, "storage_gi")} GiB
                    </span>
                  ),
                },
                {
                  key: "status",
                  label: "Status",
                  sort: (s) => servicePhase(s),
                  render: (service) => {
                    const phase = servicePhase(service);
                    const deleting = text(service, "desired") === "absent";
                    const message = text(service, "message");
                    // "service ready" under Deleting or Connecting is the operator's earlier report.
                    const showMessage = message && !(text(service, "phase") === "Ready" && phase !== "Ready");
                    const stall = stallNote(service, phase);
                    const hint = deleting ? "" : startHint(service, phase, kinds);
                    return (
                      <div className="max-w-[26rem] space-y-1">
                        <Status value={phase} tone={phaseTone(phase)} />
                        {showMessage && <div className="text-[12px] text-ink-2">{message}</div>}
                        {stall && <div className="rounded-[4px] bg-warn-soft px-2 py-1 text-[12px] text-warn">{stall}</div>}
                        {hint && (
                          <div className="flex items-start gap-1 text-[12px] text-ink-3">
                            <Timer size={12} className="mt-[3px] shrink-0" aria-hidden />
                            <span>{hint}</span>
                          </div>
                        )}
                      </div>
                    );
                  },
                },
                {
                  key: "endpoint",
                  label: "Address",
                  render: (service) => <CopyMono value={text(service, "endpoint")} short={36} label="Copy the address" />,
                },
                {
                  key: "actions",
                  label: <span className="sr-only">Actions</span>,
                  render: (service) => {
                    const deleting = text(service, "desired") === "absent";
                    const kind = text(service, "kind");
                    if (deleting) return null;
                    return (
                      <div className="flex flex-wrap justify-end gap-1.5">
                        {kind !== "zetta" && servicePhase(service) === "Ready" && <ShowConnection customerId={customerId} service={service} onShown={setShown} />}
                        <Button variant="ghost" onClick={() => setDeleting(service)} aria-label={`Delete ${text(service, "name")}`}>
                          <Trash2 size={14} aria-hidden /> Delete
                        </Button>
                      </div>
                    );
                  },
                },
              ]}
            />
          </Section>
        </>
      )}
      {catalog}
      {drawer}
      {shown && (
        <Modal title={`Connection to ${text(shown, "name")}`} onClose={() => setShown(null)} footer={<Button onClick={() => setShown(null)}>Close</Button>}>
          <Banner tone="info">Reachable inside the lake cluster, from your other services. Copy it now; it is not shown again unless you ask.</Banner>
          <dl>
            {Object.entries(shown)
              .filter(([key]) => key !== "name")
              .map(([key, value]) => (
                <div key={key} className="contents">
                  <dt>{key}</dt>
                  <dd>
                    <CopyMono value={typeof value === "string" ? value : JSON.stringify(value)} label={`Copy ${key}`} />
                  </dd>
                </div>
              ))}
          </dl>
        </Modal>
      )}
      {deleting && <DeleteService customerId={customerId} service={deleting} used={used.has(text(deleting, "name"))} onClose={() => setDeleting(null)} />}
    </>
  );
}

/** The catalog: one card per kind with its usual start time; each opens the drawer on that kind. */
function Catalog({ kinds, kindsQ, onPick }: { kinds: Kind[]; kindsQ: { isLoading: boolean; isError: boolean; error: unknown; refetch: () => unknown }; onPick: (kind: string) => void }) {
  return (
    <Section title="Service Catalog" sub="Services run beside the workspace that Design, Train and Ship use. Each kind's usual start time is on its card.">
      {kindsQ.isLoading ? (
        <Skeleton lines={3} />
      ) : kindsQ.isError ? (
        <ErrorBlock error={kindsQ.error} retry={() => void kindsQ.refetch()} what="service kinds" />
      ) : (
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-3">
          {kinds.map((k) => {
            const { icon: Icon, what } = lookOf(k.kind);
            return (
              <Card key={k.kind} tone={k.kind === "zetta" ? "active" : "default"} padded={false}>
                <div className="flex items-start gap-3 px-4 py-3">
                  <span className="flex h-9 w-9 shrink-0 items-center justify-center rounded-[6px] bg-action-soft text-action">
                    <Icon size={16} aria-hidden />
                  </span>
                  <div className="min-w-0 flex-1">
                    <div className="flex flex-wrap items-center gap-1.5">
                      <span className="text-[14px] font-semibold text-ink">{k.label}</span>
                      {k.kind === "zetta" && <Chip tone="action">default</Chip>}
                    </div>
                    {what && <div className="text-[12.5px] text-ink-3">{what}</div>}
                    <div className="mt-2 flex flex-wrap items-center justify-between gap-2">
                      <Chip title="How long a new one usually takes to become ready">
                        <Timer size={12} aria-hidden /> ready in about {spokenDuration(k.ready_seconds)}
                      </Chip>
                      <Button variant="ghost" onClick={() => onPick(k.kind)} aria-label={`New ${k.label} service`}>
                        <Plus size={14} aria-hidden /> New
                      </Button>
                    </div>
                  </div>
                </div>
              </Card>
            );
          })}
        </div>
      )}
    </Section>
  );
}

function ShowConnection({ customerId, service, onShown }: { customerId: string; service: Row; onShown: (c: Row) => void }) {
  const toast = useToast();
  const call = useAsync();
  return (
    <Button
      loading={call.busy}
      onClick={() =>
        void call.run(async () => {
          try {
            // An action, so the password never rides a URL.
            const connection = await act<Row>("service_connection", { customer_id: customerId, service_id: text(service, "id") });
            onShown({ ...connection, name: text(service, "name") });
          } catch (e) {
            toast((e as Error).message, "bad");
          }
        })
      }
    >
      <Cable size={14} aria-hidden /> Show connection
    </Button>
  );
}

/** Delete a service: the dialog names it, says what is lost, and needs its name typed. */
function DeleteService({ customerId, service, used, onClose }: { customerId: string; service: Row; used: boolean; onClose: () => void }) {
  const toast = useToast();
  const id = text(service, "id");
  const name = text(service, "name");
  const [confirm, setConfirm] = useState("");
  const del = useAsync();
  return (
    <Modal
      title={`Delete ${name}?`}
      subtitle="The service and all of its data are deleted. This cannot be undone."
      onClose={onClose}
      width={500}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="delete-service" variant="danger" loading={del.busy}>
            Delete {name}
          </Button>
        </>
      }
    >
      <Form
        id="delete-service"
        error={fieldError(del.error, "confirm") ? undefined : del.error}
        onSubmit={() =>
          del.run(async () => {
            await act("delete_service", { customer_id: customerId, service_id: id, confirm });
            setConfirm("");
            toast(`Deleting ${name}`);
            onClose();
          })
        }
      >
        {used && (
          <Banner tone="warn">
            Train → Data's connection <code>{name}</code> uses it and stops working.
          </Banner>
        )}
        <Field
          label={`Type ${name} to delete it and all of its data, which cannot be recovered`}
          error={fieldError(del.error, "confirm")}
          required
        >
          <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

function NewService({
  customerId,
  workspaceId,
  kinds,
  kindsQ,
  limit,
  initialKind,
  onClose,
}: {
  customerId: string;
  workspaceId: string;
  kinds: Kind[];
  kindsQ: { isError: boolean; error: unknown; refetch: () => unknown };
  limit: string;
  initialKind: string;
  onClose: () => void;
}) {
  const toast = useToast();
  const [kind, setKind] = useState(initialKind || "zetta");
  const [name, setName] = useState("");
  const [instances, setInstances] = useState("1");
  const [storage, setStorage] = useState("10");
  const create = useAsync();
  const chosen = kinds.find((k) => k.kind === kind);
  const Icon = lookOf(kind).icon;
  const submit = () =>
    create.run(async () => {
      const input: Row = { customer_id: customerId, workspace_id: workspaceId, kind, name };
      // Integers only when they parse, as the form handler sent them.
      for (const [field, value] of [
        ["instances", instances],
        ["storage_gi", storage],
      ] as const) {
        if (/^-?\d+$/.test(value)) input[field] = Number(value);
      }
      await act("create_service", input);
      setName("");
      toast(`Creating ${name}`);
      onClose();
    });
  return (
    <Drawer
      title={chosen ? `New ${chosen.label} Service` : "New Service"}
      subtitle={chosen ? `Usually ready in about ${spokenDuration(chosen.ready_seconds)}.` : undefined}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          {workspaceId && (
            <Button type="submit" form="new-service" variant="primary" loading={create.busy}>
              Create service
            </Button>
          )}
        </>
      }
    >
      {!workspaceId ? (
        <Empty compact icon={Icon} title="Choose a workspace first" action={<Button to="/workspaces">Workspaces</Button>}>
          A service is created in a workspace. <Link to="/workspaces">Choose or create a workspace</Link> to create one.
        </Empty>
      ) : (
        <Form id="new-service" onSubmit={submit} error={["kind", "name", "instances", "storage_gi"].some((f) => fieldError(create.error, f)) ? undefined : create.error}>
          {kindsQ.isError && <ErrorBlock error={kindsQ.error} retry={() => void kindsQ.refetch()} what="service kinds" />}
          <Field label="Kind" error={fieldError(create.error, "kind")}>
            <select name="kind" value={kind} onChange={(e) => setKind(e.target.value)}>
              {kinds.map((k) => (
                <option key={k.kind} value={k.kind}>
                  {k.label} — ready in about {spokenDuration(k.ready_seconds)}
                </option>
              ))}
            </select>
          </Field>
          <Field label="Name" help="Lowercase letters, digits and dashes, starting with a letter." error={fieldError(create.error, "name")} required>
            <input name="name" pattern="[a-z][a-z0-9\-]{0,30}" placeholder="crm" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Instances" error={fieldError(create.error, "instances")}>
            <select name="instances" value={instances} onChange={(e) => setInstances(e.target.value)}>
              <option>1</option>
              <option>2</option>
              <option>3</option>
            </select>
          </Field>
          <Field label="Storage (GiB)" error={fieldError(create.error, "storage_gi")}>
            <input name="storage_gi" type="number" min={1} max={100} value={storage} onChange={(e) => setStorage(e.target.value)} />
          </Field>
          {limit && limit !== "0" && <Banner tone="quiet">Your workspaces may run {limit} services at once.</Banner>}
        </Form>
      )}
    </Drawer>
  );
}
