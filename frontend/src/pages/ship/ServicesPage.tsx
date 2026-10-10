/** Services (web_ui.cpp services_content): every database, stream and engine the space runs on
 * lake. Not a workspace page: the services belong to the space. A connection is shown in a dialog
 * when asked for, never in the URL, and not kept. */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { ApiError, act, useRead, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, ErrorBlock, Field, Form, Page, Skeleton, fieldError } from "@/lib/ui";
import { qs, secondsSince, spokenDuration, text, useAsync, wq } from "../train/workspace";

const SUB =
  "The databases, streams and engines this space runs on lake: Postgres, Zetta, Kafka, ClickHouse, TiDB, Cassandra and Valkey, each its own, with its data kept apart from every other customer's. Your workspace is the Zetta that Design, Train and Ship use.";

type Kind = { kind: string; label: string; ready_seconds: number };

/** The status a service shows (service_phase): list_services' `status`, else derived. */
export function servicePhase(service: Row): string {
  const status = text(service, "status");
  if (status) return status;
  const phase = text(service, "phase");
  if (text(service, "desired") === "absent") return !phase || phase === "Ready" ? "Deleting" : phase;
  if (phase === "Ready" && service.connected === false) return "Connecting";
  return phase || "Waiting for the operator";
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
  useTrail([
    { label: "Ship", to: `/ship/models?${qs({ customer_id: customerId })}` },
    { label: "Services", to: `/services?${qs({ customer_id: customerId })}` },
  ]);
  return (
    <Page eyebrow="Ship" title="Services" sub={SUB}>
      {customerId ? <Services customerId={customerId} /> : <div className="panel empty">You have no space to work in yet.</div>}
    </Page>
  );
}

function Services({ customerId }: { customerId: string }) {
  const { workspaceId, workspaces } = useSession();
  // The current workspace, when it belongs to this space: its connections that use a service.
  const workspace = workspaces.find((w) => w.id === workspaceId && w.customer_id === customerId)?.id ?? "";
  const kindsQ = useRead<{ kinds?: Kind[] }>("list_service_kinds", undefined, { staleTime: 3_600_000 });
  const kinds = kindsQ.data?.kinds ?? [];
  const listed = useRead<{ services?: Row[]; limit?: number | string }>(
    "list_services",
    { customer_id: customerId },
    {
      workspace: null,
      refetchInterval: (query) => {
        const services = (query.state.data as { services?: Row[] } | undefined)?.services ?? [];
        return services.some((s) => {
          const phase = servicePhase(s);
          return phase !== "Ready" && phase !== "Failed" && phase !== "Unassigned";
        })
          ? 3_000
          : false;
      },
    },
  );
  const connections = useQuery({
    queryKey: ["workspace_query", customerId, workspace, "list_connections", {}],
    queryFn: ({ signal }) => wq<{ connections?: Row[] }>({ customerId, workspaceId: workspace }, "list_connections", {}, signal),
    enabled: !!workspace && listed.isSuccess,
  });
  const [shown, setShown] = useState<Row | null>(null);

  if (listed.isLoading) return <Skeleton />;
  if (listed.isError) {
    const e = listed.error;
    const unconfigured = e instanceof ApiError && e.code === "store_unavailable" && /not configured/.test(e.message);
    return unconfigured ? (
      <div className="panel empty">Services are not available on this lake yet: the operator has not enabled them.</div>
    ) : e instanceof ApiError && e.status >= 400 && e.status < 500 ? (
      <div className="panel empty">{e.message}</div>
    ) : (
      <ErrorBlock error={e} retry={() => void listed.refetch()} what="services" />
    );
  }
  const all = listed.data?.services ?? [];
  // Zetta services first (the workspace is one), then the rest in lake's order.
  const services = [...all.filter((s) => text(s, "kind") === "zetta"), ...all.filter((s) => text(s, "kind") !== "zetta")];
  const settling = services.some((s) => {
    const phase = servicePhase(s);
    return phase !== "Ready" && phase !== "Failed" && phase !== "Unassigned";
  });
  const used = new Set((connections.data?.connections ?? []).map((c) => text(c, "name")));
  const limit = String(listed.data?.limit ?? "");

  return (
    <>
      {services.length === 0 ? (
        <div className="panel empty">No services yet.</div>
      ) : (
        <div className="table-wrap" role="region" aria-label="Services" tabIndex={0}>
          <table>
            <thead>
              <tr>
                <th>Name</th>
                <th>Kind</th>
                <th>Size</th>
                <th>Status</th>
                <th>Address</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              {services.map((service) => {
                const id = text(service, "id");
                const name = text(service, "name");
                const kind = text(service, "kind");
                const phase = servicePhase(service);
                const deleting = text(service, "desired") === "absent";
                const message = text(service, "message");
                // "service ready" under Deleting or Connecting is the operator's earlier report.
                const showMessage = message && !(text(service, "phase") === "Ready" && phase !== "Ready");
                const stall = stallNote(service, phase);
                const hint = deleting ? "" : startHint(service, phase, kinds);
                return (
                  <tr key={id}>
                    <td>
                      {name}
                      {service.default === true && <span className="muted"> (workspace)</span>}
                    </td>
                    <td>{kinds.find((k) => k.kind === kind)?.label ?? kind}</td>
                    <td>
                      {text(service, "instances")} × {text(service, "storage_gi")} GiB
                    </td>
                    <td>
                      {phase}
                      {showMessage && (
                        <>
                          <br />
                          <span className="muted">{message}</span>
                        </>
                      )}
                      {stall && (
                        <>
                          <br />
                          <span className="notice status-warning">{stall}</span>
                        </>
                      )}
                      {hint && (
                        <>
                          <br />
                          <span className="muted">{hint}</span>
                        </>
                      )}
                    </td>
                    <td>
                      <code>{text(service, "endpoint")}</code>
                    </td>
                    <td>
                      {!deleting && kind !== "zetta" && phase === "Ready" && <ShowConnection customerId={customerId} service={service} onShown={setShown} />}
                      {!deleting && <DeleteService customerId={customerId} id={id} name={name} used={used.has(name)} />}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
      {settling && <p className="muted">This page checks again every 3 seconds while a service is changing.</p>}
      <NewService customerId={customerId} workspaceId={workspace} kinds={kinds} kindsQ={kindsQ} limit={limit} />
      {shown && (
        <Modal title={`Connection to ${text(shown, "name")}`} onClose={() => setShown(null)} footer={<Button onClick={() => setShown(null)}>Close</Button>}>
          <p>Reachable inside the lake cluster, from your other services. Copy it now; it is not shown again unless you ask.</p>
          <dl>
            {Object.entries(shown)
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
    </>
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
      Show connection
    </Button>
  );
}

function DeleteService({ customerId, id, name, used }: { customerId: string; id: string; name: string; used: boolean }) {
  const toast = useToast();
  const [confirm, setConfirm] = useState("");
  const del = useAsync();
  return (
    <details>
      <summary>Delete</summary>
      <Form
        error={fieldError(del.error, "confirm") ? undefined : del.error}
        onSubmit={() =>
          del.run(async () => {
            await act("delete_service", { customer_id: customerId, service_id: id, confirm });
            setConfirm("");
            toast(`Deleting ${name}`);
          })
        }
      >
        {used && (
          <p className="notice status-warning">
            Train → Data's connection <code>{name}</code> uses it and stops working.
          </p>
        )}
        <Field
          label={
            <>
              Type <code>{name}</code> to delete it and all of its data, which cannot be recovered
            </>
          }
          error={fieldError(del.error, "confirm")}
          required
        >
          <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="danger" loading={del.busy}>
            Delete {name}
          </Button>
        </div>
      </Form>
    </details>
  );
}

function NewService({
  customerId,
  workspaceId,
  kinds,
  kindsQ,
  limit,
}: {
  customerId: string;
  workspaceId: string;
  kinds: Kind[];
  kindsQ: { isError: boolean; error: unknown; refetch: () => unknown };
  limit: string;
}) {
  const toast = useToast();
  const [kind, setKind] = useState("zetta");
  const [name, setName] = useState("");
  const [instances, setInstances] = useState("1");
  const [storage, setStorage] = useState("10");
  const create = useAsync();
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
    });
  return (
    <section className="panel">
      <h2>New Service</h2>
      {!workspaceId ? (
        <p className="muted">
          A service is created in a workspace. <Link to="/workspaces">Choose or create a workspace</Link> to create one.
        </p>
      ) : (
        <Form onSubmit={submit} error={["kind", "name", "instances", "storage_gi"].some((f) => fieldError(create.error, f)) ? undefined : create.error}>
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
          <Field label="Name" error={fieldError(create.error, "name")} required>
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
          <p className="muted">
            {limit && limit !== "0" ? `This space may run ${limit} services at once. ` : ""}Each kind's usual start time is next to it.
          </p>
          <div>
            <Button type="submit" variant="primary" loading={create.busy}>
              Create service
            </Button>
          </div>
        </Form>
      )}
    </section>
  );
}
