/** Data → Drives → Storage (`/w/<id>/drives?tab=storage`, lake#656, docs/ui/drives.md#storage): the
 * workspace's storage, moved from Datasets. Figures, the table (type and provider, size with the
 * used space, who mounts it, this month's volume time, status with the operator's message,
 * endpoint), New storage in a dialog, the connection in a dialog, and delete with the name typed.
 * A bucket or filesystem opens as a drive (a `store` drive); a volume is shown, not browsed, and
 * links to the service or app that mounts it (lake#666). Shown whether or not the workspace is
 * ready: storage does not need it. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { Box, Gauge, HardDrive, Plus, Timer } from "lucide-react";
import { act, useRead, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, Form, Mono, Query, Stat, StatRow, Status } from "@/lib/ui";
import { qs, text, useAsync, useSpaceRead, useWorkspace, useWsPath } from "@/pages/train/workspace";
import { formatValue, usageLink, usageOf, windows, type Reading } from "@/pages/usage/usage";
import type { Drive } from "./storage";
import { browsable, driveForStore, mountedBy, phaseTone, storeFigures, storeKindWords, storePhase, storeSettled, usedWords, workloadHref } from "./stores";

/** The workspace's stores, checked every 3 seconds while one is changing. */
export function useStores() {
  return useSpaceRead<{ stores?: Row[] }>("list_stores", {}, {
    refetchInterval: (query) => {
      const list = ((query.state.data as { stores?: Row[] } | undefined)?.stores ?? []) as Row[];
      return list.some((s) => !storeSettled(s)) ? 3_000 : false;
    },
  });
}

export function StorageTab({ drives, onOpenAsDrive }: { drives: Drive[]; onOpenAsDrive: (store: Row) => void }) {
  const scope = useWorkspace();
  const wsPath = useWsPath();
  const toast = useToast();
  const stores = useStores();
  const providers = useSpaceRead<{ providers?: Row[] }>("list_store_providers", {}, { workspace: false, staleTime: 300_000 });
  const providerList = providers.data?.providers ?? [];
  const list = stores.data?.stores ?? [];
  const figures = storeFigures(list);
  // Each store's volume time this month, from its workspace's usage (lake#564).
  const month = windows("month").current;
  const usage = useRead<Reading>(
    list.length ? "usage_over_time" : null,
    { customer_id: scope.customerId, dimensions: ["volumes"], from: month.from, to: month.to, granularity: "month" },
    { workspace: null, staleTime: 300_000 },
  );
  const show = useAsync();
  const [connection, setConnection] = useState<Row | null>(null);
  const [creating, setCreating] = useState(false);
  const [deleting, setDeleting] = useState<Row | null>(null);
  const newButton = (
    <Button onClick={() => setCreating(true)}>
      <Plus size={14} aria-hidden /> New storage
    </Button>
  );
  return (
    <section aria-label="Storage" id="storage">
      <StatRow cols={4}>
        <Stat label="Storage" icon={HardDrive} value={stores.data ? figures.count : "—"} sub={figures.changing ? `${figures.changing} changing` : "buckets, filesystems, volumes"} />
        <Stat label="Opens as a drive" icon={Box} value={stores.data ? figures.browsable : "—"} sub="buckets and filesystems" />
        <Stat label="Volumes" icon={Gauge} value={stores.data ? figures.volumes : "—"} sub="shown, not browsed" />
        <Stat label="Size" icon={Timer} value={stores.data ? `${figures.sizeGi} GiB` : "—"} sub="asked for, in all" />
      </StatRow>
      <Card label="Storage" title="Buckets, Filesystems and Volumes" actions={newButton}>
        <p className="muted mt-0 text-[12.5px]">
          Storage for your services and apps, private to this workspace. A bucket or filesystem opens as a drive; a volume belongs to the service that mounts it. Each one's volume time is on{" "}
          <Link to={usageLink({ dimension: "volumes" })}>Usage</Link>.
        </p>
        {show.error ? <ErrorBlock error={show.error} /> : null}
        <Query q={stores} what="storage">
          {(value) =>
            (value.stores ?? []).length === 0 ? (
              <Empty compact icon={HardDrive} title="No storage yet" action={<Button onClick={() => setCreating(true)}>New storage</Button>}>
                Create a volume your services can mount.
              </Empty>
            ) : (
              <DataTable<Row>
                rows={value.stores ?? []}
                what="storage"
                caption="Storage"
                rowKey={(s, i) => text(s, "id") || String(i)}
                columns={[
                  {
                    key: "name",
                    label: "Storage",
                    sort: (s) => text(s, "name"),
                    render: (store) => {
                      const [type, provider] = storeKindWords(providerList, text(store, "type"), text(store, "provider"));
                      return (
                        <div className="min-w-0">
                          <div className="font-semibold text-ink">{text(store, "name")}</div>
                          <div className="text-[12px] text-ink-3">
                            {type} · {provider}
                          </div>
                        </div>
                      );
                    },
                  },
                  {
                    key: "size",
                    label: "Size",
                    numeric: true,
                    sort: (s) => Number(s.size_gi) || 0,
                    render: (s) => (
                      <div className="whitespace-nowrap">
                        <span className="font-mono">{text(s, "size_gi")} GiB</span>
                        <div className="text-[12px] text-ink-3">{usedWords(s)}</div>
                      </div>
                    ),
                  },
                  {
                    key: "mounted",
                    label: "Mounted By",
                    sort: (s) => mountedBy(s).map((m) => m.name).join(","),
                    render: (store) => {
                      const mounts = mountedBy(store);
                      if (!mounts.length) return <span className="text-[12.5px] text-ink-3">Not mounted</span>;
                      return (
                        <ul className="m-0 list-none space-y-0.5 p-0">
                          {mounts.map((m) => (
                            <li key={`${m.kind}:${m.id}`} className="text-[12.5px]">
                              <Link to={workloadHref(m, scope.customerId, scope.workspaceId)}>{m.name || m.id}</Link>{" "}
                              <span className="text-ink-3">
                                {m.kind === "app" ? "app" : "service"}
                                {m.mount_path ? ` · ${m.mount_path}` : ""}
                                {m.read_only ? " · read-only" : ""}
                              </span>
                            </li>
                          ))}
                        </ul>
                      );
                    },
                  },
                  {
                    key: "month",
                    label: "This Month",
                    render: (store) => {
                      if (!usage.data) return <span className="text-ink-3">—</span>;
                      const [volumes] = usageOf(usage.data, [{ dimension: "volumes", breakdown: "by_object", keys: [text(store, "id")] }]);
                      return (
                        <Link to={usageLink({ dimension: "volumes" })} className="font-mono text-[12.5px] whitespace-nowrap tabular-nums text-inherit no-underline hover:underline">
                          {formatValue(volumes?.value, volumes?.unit ?? "")}
                        </Link>
                      );
                    },
                  },
                  {
                    key: "status",
                    label: "Status",
                    sort: (s) => storePhase(s),
                    render: (store) => {
                      const phase = storePhase(store);
                      return (
                        <div className="min-w-0">
                          <Status value={phase} tone={phaseTone(phase)} />
                          {text(store, "message") && <div className="mt-0.5 text-[12px] text-ink-3">{text(store, "message")}</div>}
                        </div>
                      );
                    },
                  },
                  { key: "endpoint", label: "Endpoint", render: (s) => <Mono>{text(s, "endpoint")}</Mono> },
                  {
                    key: "actions",
                    label: "",
                    className: "text-right",
                    render: (store) => {
                      const id = text(store, "id");
                      const name = text(store, "name");
                      const phase = storePhase(store);
                      if (text(store, "desired") === "absent") return null;
                      const kind = browsable(store);
                      const drive = driveForStore(drives, id);
                      return (
                        <div className="flex flex-wrap items-center justify-end gap-1.5">
                          {kind === "volume" && <Chip>Shown, not browsed</Chip>}
                          {kind === "yes" &&
                            (drive ? (
                              <Button variant="ghost" to={wsPath(`/drives/drive?${qs({ id: drive.id })}`)}>
                                Open drive
                              </Button>
                            ) : (
                              <Button variant="ghost" onClick={() => onOpenAsDrive(store)}>
                                Open as a drive
                              </Button>
                            ))}
                          {phase === "Ready" && (
                            <Button
                              variant="ghost"
                              loading={show.busy}
                              onClick={() =>
                                void show.run(async () => {
                                  const answer = await act<Row>("store_connection", { customer_id: scope.customerId, workspace_id: scope.workspaceId, store_id: id });
                                  setConnection({ ...answer, name });
                                })
                              }
                            >
                              Show connection
                            </Button>
                          )}
                          <Button variant="ghost" onClick={() => setDeleting(store)}>
                            Delete
                          </Button>
                        </div>
                      );
                    },
                  },
                ]}
              />
            )
          }
        </Query>
        {list.some((s) => !storeSettled(s)) && <Banner tone="quiet">This list checks again every 3 seconds while storage is changing.</Banner>}
      </Card>
      {creating && <NewStorage providers={providerList} providersError={providers.isError ? providers.error : null} onClose={() => setCreating(false)} />}
      {deleting && <DeleteStore store={deleting} onClose={() => setDeleting(null)} onDeleted={() => toast(`Deleting ${text(deleting, "name")}`)} />}
      {connection && (
        <Modal title={`Connection to ${text(connection, "name")}`} subtitle="What a service in your workspace mounts or calls to use it. It is not shown again unless you ask." onClose={() => setConnection(null)}>
          <dl>
            {Object.entries(connection)
              .filter(([key]) => key !== "name")
              .map(([key, value]) => (
                <div key={key} className="contents">
                  <dt>{key}</dt>
                  <dd>
                    <code>{typeof value === "string" ? value : JSON.stringify(value)}</code>
                  </dd>
                </div>
              ))}
          </dl>
        </Modal>
      )}
    </section>
  );
}

/** Delete storage: the person types its name, which the server checks too. Storage a service or
 * an app mounts is not deleted: the dialog names them, and Lake refuses it too (lake#666). */
function DeleteStore({ store, onClose, onDeleted }: { store: Row; onClose: () => void; onDeleted: () => void }) {
  const scope = useWorkspace();
  const id = text(store, "id");
  const name = text(store, "name");
  const mounts = mountedBy(store);
  const remove = useAsync();
  const [confirm, setConfirm] = useState("");
  return (
    <Modal
      title={`Delete ${name}?`}
      onClose={onClose}
      width={460}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" type="submit" form="delete-store" loading={remove.busy} disabled={confirm !== name || mounts.length > 0}>
            Delete {name}
          </Button>
        </>
      }
    >
      {mounts.length > 0 ? (
        <Banner tone="warn">
          {mounts.map((m, i) => (
            <span key={`${m.kind}:${m.id}`}>
              {i > 0 ? (i === mounts.length - 1 ? " and " : ", ") : ""}
              {i === 0 ? "The" : "the"} {m.kind} <Link to={workloadHref(m, scope.customerId, scope.workspaceId)}>{m.name || m.id}</Link>
            </span>
          ))}{" "}
          {mounts.length === 1 ? "mounts" : "mount"} it. Remove {mounts.length === 1 ? "the mount" : "the mounts"} first, in the service's or app's storage.
        </Banner>
      ) : (
        <Banner tone="warn">Deleting storage deletes the data in it, which cannot be recovered. A drive that opens it stops working; delete that drive too.</Banner>
      )}
      <Form
        id="delete-store"
        error={remove.error}
        onSubmit={() =>
          remove.run(async () => {
            await act("delete_store", { customer_id: scope.customerId, workspace_id: scope.workspaceId, store_id: id, confirm });
            setConfirm("");
            onDeleted();
            onClose();
          })
        }
      >
        <label>
          <span>
            Type <code>{name}</code> to delete it
          </span>
          <input name="confirm" required autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </label>
      </Form>
    </Modal>
  );
}

function NewStorage({ providers, providersError, onClose }: { providers: Row[]; providersError: unknown; onClose: () => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const create = useAsync();
  const available = providers.filter((p) => p.available);
  const more = providers.some((p) => !p.available);
  const [store, setStore] = useState("");
  const [name, setName] = useState("");
  const [size, setSize] = useState("10");
  const chosen = store || (available[0] ? `${text(available[0], "type")}:${text(available[0], "provider")}` : "");
  return (
    <Modal
      title="New Storage"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="new-storage" loading={create.busy}>
            Create storage
          </Button>
        </>
      }
    >
      {providersError ? <ErrorBlock error={providersError} what="storage providers" /> : null}
      <Form
        id="new-storage"
        error={create.error}
        onSubmit={() =>
          create.run(async () => {
            const colon = chosen.indexOf(":");
            const input: Row = {
              customer_id: scope.customerId,
              workspace_id: scope.workspaceId,
              type: colon < 0 ? chosen : chosen.slice(0, colon),
              provider: colon < 0 ? "" : chosen.slice(colon + 1),
              name,
            };
            if (/^-?\d+$/.test(size)) input.size_gi = Number(size);
            await act("create_store", input);
            toast(`Creating ${name}`);
            setName("");
            onClose();
          })
        }
      >
        <Field label="Type and provider" help={more ? "Buckets (Cloudflare R2, AWS S3, Backblaze B2, Azure, Google Cloud) and shared filesystems are coming later." : undefined}>
          <select name="store" value={chosen} onChange={(e) => setStore(e.target.value)}>
            {available.map((p, i) => (
              <option key={i} value={`${text(p, "type")}:${text(p, "provider")}`}>
                {text(p, "label")}
              </option>
            ))}
          </select>
        </Field>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-[minmax(0,1fr)_8rem]">
          <Field label="Name">
            <input name="name" required pattern="[a-z][a-z0-9\-]{0,30}" placeholder="uploads" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Size (GiB)">
            <input name="size_gi" type="number" min={1} max={100} value={size} onChange={(e) => setSize(e.target.value)} />
          </Field>
        </div>
      </Form>
    </Modal>
  );
}
