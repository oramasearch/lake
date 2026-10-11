/** Ship → Apps → one app (web_ui.cpp web_ui_app; handlers serve_app_page and the nineteen
 * serve_app_form actions), as a Split: tabs for Overview (deploy history with rollback), Builds,
 * Logs, Events (live, by long poll), Settings (environment and authentication) and Services
 * (bindings); a side column with the status and rollout track, scale, deploy, lifecycle controls
 * and deletion. Every control calls its capability directly. */
import { type FormEvent, type ReactNode, useEffect, useMemo, useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { ExternalLink, Hammer, Play, Plus, RotateCcw, Square, Undo2 } from "lucide-react";
import { act, read, useList, useRead, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Button, Card, type Column, DataTable, Empty, ErrorBlock, KeyValue, Page, Skeleton, Split, Status } from "@/lib/ui";
import { envInput, secretEnvLabel } from "./secretEnv";
import { AppPhase, AppUrl, decimal, formValues } from "./AppsPage";
import { PageError, pause, qs, QueryTabs, storedNumber, storeNumber, streamHref, Terminal, useQueryTab, useRunner, useWorkspaceLink } from "./shared";
import { stopsPolling } from "@/lib/errors";
import { UsageThisMonth } from "../usage/UsageThisMonth";

type AppEvent = Row & { sequence?: number; type?: string; message?: string; status?: Row };

function eventLine(event: AppEvent): string {
  return `${event.sequence ?? ""}  ${event.type || "event"}  ${event.message || JSON.stringify(event.status ?? {})}`;
}

const TABS = ["overview", "builds", "logs", "events", "settings", "services"] as const;
type Tab = (typeof TABS)[number];

/** Every retained app event, then the new ones as they arrive (`receive_app_events`, long poll). */
function useAppEvents(customerId: string, workspaceId: string | null, appId: string): { events: AppEvent[]; retained: number } {
  const [events, setEvents] = useState<AppEvent[]>([]);
  const [retained, setRetained] = useState(0);
  useEffect(() => {
    if (!customerId || !workspaceId || !appId) return;
    const controller = new AbortController();
    const signal = controller.signal;
    const key = `lake-app-events:${appId}`;
    setEvents([]);
    setRetained(0);
    void (async () => {
      let next = 1;
      let wait = 0;
      while (!signal.aborted) {
        try {
          const value = await read<{ events?: AppEvent[]; next_sequence?: number }>(
            "receive_app_events",
            { customer_id: customerId, workspace_id: workspaceId, app_id: appId, from_sequence: next, wait_ms: wait },
            { signal },
          );
          const batch = value.events ?? [];
          if (batch.length) setEvents((current) => [...current, ...batch]);
          if (wait === 0) {
            // What the page showed when it rendered; later events are live.
            setRetained(batch.length);
            next = Math.max(next, storedNumber(key));
            wait = 25000;
          }
          next = Math.max(next, Number(value.next_sequence) || next);
          storeNumber(key, next);
        } catch (error) {
          // An answer asking again cannot change (not set up, unavailable, refused) ends the
          // follow; anything else is retried after a pause (lib/errors.ts, lake#575).
          if (signal.aborted || stopsPolling(error)) break;
          await pause(2000, signal);
        }
      }
    })();
    return () => {
      controller.abort();
    };
  }, [customerId, workspaceId, appId]);
  return { events, retained };
}

export default function AppPage() {
  const { space, workspaceId } = useSession();
  const ws = useWorkspaceLink();
  const [params] = useSearchParams();
  const customerId = space?.id ?? "";
  const appId = params.get("app_id") ?? "";
  const tab = useQueryTab<Tab>(TABS);
  const runner = useRunner();

  const appQuery = useRead<Row>(customerId && appId ? "get_app" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined, app_id: appId });
  const app = appQuery.data;
  const name = String(app?.name ?? "");
  useTrail(app ? [{ label: "Ship", to: ws("/ship/models") }, { label: "Apps", to: ws("/ship/apps") + qs({ customer_id: customerId }) }, entityCrumb(name)] : null, app ? name : undefined);

  const { events, retained } = useAppEvents(customerId, workspaceId, appId);
  const logs = useRead<{ items?: Row[] }>(customerId && appId ? "app_logs" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined, app_id: appId, limit: 200 });
  const builds = useRead<{ builds?: Row[] }>(customerId ? "list_builds" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined });
  const services = useList<Row>(customerId ? "list_services" : null, { customer_id: customerId }, { pick: "services" });

  if (!customerId || !appId) return <Navigate to={ws("/ship/apps") + qs({ customer_id: customerId })} replace />;
  if (appQuery.isLoading) {
    return (
      <Page eyebrow="Ship · Apps" title="App">
        <Skeleton />
      </Page>
    );
  }
  if (appQuery.isError || !app) {
    return (
      <Page eyebrow="Ship · Apps" title="App">
        <ErrorBlock error={appQuery.error} retry={() => void appQuery.refetch()} what="this app" />
      </Page>
    );
  }

  const base = { customer_id: customerId, workspace_id: workspaceId, app_id: appId };
  const call = (key: string, capability: string, input: Row, done: string) => runner.run(key, () => act(capability, { ...base, ...input }), done);
  const source = (app.source && typeof app.source === "object" ? app.source : {}) as Row;
  const runtime = app.mode === "runtime";

  // The live phase: the latest event since the page loaded that carried one, else the app's own.
  const livePhase = events.slice(retained).reverse().find((event) => event.status?.phase)?.status?.phase as string | undefined;
  const phase = livePhase ?? (app.phase as string | undefined);
  const ready = livePhase ? livePhase === "Running" || livePhase === "Ready" : !!app.ready;

  const history = [...events].reverse().filter((event) => event.type === "deploy" || event.type === "rollback" || event.type === "create");

  const appBuilds = (builds.data?.builds ?? []).filter(
    (build) => String(build.repository ?? "") === String(source.repository ?? "") && String(build.target ?? "").includes(`/${name}:`) && (build.id || build.build_id),
  );

  const env = (app.env && typeof app.env === "object" ? app.env : {}) as Record<string, unknown>;
  const secretEnv = (app.secret_env_refs && typeof app.secret_env_refs === "object" ? app.secret_env_refs : {}) as Record<string, unknown>;
  const bindings = (app.bindings && typeof app.bindings === "object" ? app.bindings : {}) as Row;
  const hasZetta = bindings.zetta !== undefined && bindings.zetta !== null;
  const boundServices = (Array.isArray(bindings.services) ? bindings.services : []).filter((id: unknown): id is string => typeof id === "string");
  const envCount = Object.keys(env).length + Object.keys(secretEnv).length;

  const submit = (_key: string, handler: (form: Record<string, string>) => Promise<unknown>) => (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void handler(form);
  };

  const url = typeof app.url === "string" && /^https?:\/\//.test(app.url) ? app.url : "";
  const tabHref = (id: Tab) => {
    const next = new URLSearchParams(params);
    next.set("tab", id);
    return `?${next.toString()}`;
  };

  return (
    <Page
      eyebrow="Ship · Apps"
      title={
        <span className="inline-flex flex-wrap items-center gap-2.5">
          {name} <AppPhase phase={phase} ready={ready} />
        </span>
      }
      sub={<AppUrl url={app.url} />}
      actions={
        <>
          {url && (
            <Button href={url}>
              <ExternalLink size={14} aria-hidden /> Open app
            </Button>
          )}
          <Button to={streamHref(ws, customerId, `app-events.${appId}`)}>Event stream</Button>
        </>
      }
    >
      <PageError error={runner.error} />
      <Split
        side={
          <>
            <Card label="Status" title="Rollout">
              <div className="app-phase-track" aria-label="Rollout progress">
                <span className={ready ? "app-phase-fill ready" : "app-phase-fill"} />
              </div>
              <KeyValue
                rows={[
                  ["Phase", <AppPhase key="p" phase={phase} ready={ready} />],
                  ["Desired", String(app.desired ?? "")],
                  ["Generation", <span key="g" className="font-mono">{`${String(app.generation ?? "")} / observed ${String(app.observed_generation ?? "")}`}</span>],
                  ["URL", <AppUrl key="u" url={app.url} />],
                  ["Mode", String(app.mode ?? "")],
                  ["Framework", String(app.framework ?? "")],
                  ["Port", <span key="port" className="font-mono">{String(app.port ?? "")}</span>],
                  ["Access", app.auth === "none" ? "Public" : "Lake sign-in"],
                  ["Updated", relative(app.updated_at)],
                ]}
              />
              {app.message ? <p className="muted mb-0 text-[13px]">{String(app.message)}</p> : null}
            </Card>

            <Card label="Scale" title="Replicas">
              <form className="row-form flex flex-wrap items-end gap-2" onSubmit={submit("scale", (form) => call("scale", "scale_app", { replicas: decimal(form.replicas) }, "Scaled"))}>
                <label className="min-w-0 flex-1">
                  Replicas
                  <input type="number" name="replicas" min={1} max={100} defaultValue={String(app.replicas ?? "")} key={String(app.replicas ?? "")} />
                </label>
                <Button type="submit" loading={runner.busy === "scale"}>
                  Scale
                </Button>
              </form>
            </Card>

            <Card label="Actions" title="Deploy and Lifecycle">
              <form
                className="stack"
                onSubmit={submit("deploy", (form) =>
                  call(
                    "deploy",
                    "deploy_app",
                    runtime
                      ? { source: { repository: String(source.repository ?? ""), revision: form.revision ?? "", path: String(source.path ?? ""), entrypoint: String(source.entrypoint ?? "") } }
                      : { image: form.image ?? "" },
                    "Deploy requested",
                  ),
                )}
              >
                {runtime ? (
                  <label>
                    Deploy revision
                    <input name="revision" defaultValue={String(source.revision ?? "")} key={String(source.revision ?? "")} required />
                  </label>
                ) : (
                  <label>
                    Deploy image
                    <input name="image" defaultValue={String(app.image ?? "")} key={String(app.image ?? "")} required />
                  </label>
                )}
                <div>
                  <Button variant="primary" type="submit" loading={runner.busy === "deploy"}>
                    Deploy
                  </Button>
                </div>
              </form>
              <div className="app-controls mt-3 border-t border-hairline pt-3">
                <ActionButton busy={runner.busy} id="restart" onClick={() => call("restart", "restart_app", {}, "Restart requested")}>
                  <RotateCcw size={14} aria-hidden /> Restart
                </ActionButton>
                <ActionButton busy={runner.busy} id="stop" onClick={() => call("stop", "stop_app", {}, "Stop requested")}>
                  <Square size={14} aria-hidden /> Stop
                </ActionButton>
                <ActionButton busy={runner.busy} id="start" onClick={() => call("start", "start_app", {}, "Start requested")}>
                  <Play size={14} aria-hidden /> Start
                </ActionButton>
              </div>
            </Card>

            <UsageThisMonth
              customerId={customerId}
              workspaceId={workspaceId ?? ""}
              match={[
                { dimension: "cpu", breakdown: "by_object", keys: [appId], label: "CPU" },
                { dimension: "ram", breakdown: "by_object", keys: [appId], label: "RAM" },
                { dimension: "gpu", breakdown: "by_object", keys: [appId], label: "GPU" },
                { dimension: "instances", breakdown: "by_object", keys: [appId], label: "Instance time" },
              ]}
              note="CPU, RAM and GPU are what the app's replicas are scheduled with, for as long as they run."
            />
            <Card label="Danger zone" title="Delete App" tone="exception">
              <form className="row-form flex flex-col gap-3" onSubmit={submit("delete", (form) => call("delete", "delete_app", { confirm: form.confirm ?? "" }, `Deletion of ${name} requested`))}>
                <p className="m-0 text-[13px] text-ink-2">
                  Type <code>{name}</code> to delete
                </p>
                <label>
                  App name
                  <input name="confirm" required autoComplete="off" />
                </label>
                <div>
                  <Button variant="danger" type="submit" loading={runner.busy === "delete"}>
                    Delete app
                  </Button>
                </div>
              </form>
            </Card>
          </>
        }
      >
        <div>
          <QueryTabs<Tab>
            value={tab}
            tabs={[
              { id: "overview", label: "Overview", count: history.length },
              { id: "builds", label: "Builds", count: builds.isError ? undefined : appBuilds.length },
              { id: "logs", label: "Logs" },
              { id: "events", label: "Events", count: events.length },
              { id: "settings", label: "Settings", count: envCount },
              { id: "services", label: "Services", count: boundServices.length + (hasZetta ? 1 : 0) },
            ]}
          />

          {tab === "overview" && (
            <div className="space-y-4">
              <Card title="Deploy History" padded={false}>
                <HistoryTable history={history} busy={runner.busy} call={call} />
              </Card>
              <Terminal
                title="Latest events"
                live
                actions={
                  <Link to={tabHref("events")} replace className="text-inherit">
                    All events
                  </Link>
                }
                empty="Waiting for app events…"
              >
                {events.slice(-8).map((event, index) => (
                  <div key={`${event.sequence}-${index}`}>{eventLine(event)}</div>
                ))}
              </Terminal>
            </div>
          )}

          {tab === "builds" && (
            <div className="space-y-3">
              {builds.isLoading ? (
                <Skeleton lines={2} />
              ) : builds.isError ? (
                <Empty
                  icon={Hammer}
                  title="Builds unavailable"
                  action={
                    <Button disabled>
                      <Hammer size={14} aria-hidden /> Build
                    </Button>
                  }
                >
                  Builds are not available on this Lake yet.
                </Empty>
              ) : (
                <>
                  <div className="flex flex-wrap items-center justify-between gap-2">
                    <p className="muted m-0 text-[13px]">Image builds of this app's repository.</p>
                    <ActionButton
                      busy={runner.busy}
                      id="build"
                      primary
                      onClick={() => runner.run("build", () => act("build_app", { customer_id: customerId, workspace_id: workspaceId, app: appId }), "Build queued")}
                    >
                      <Hammer size={14} aria-hidden /> {appBuilds.length === 0 ? "Build" : "Rebuild"}
                    </ActionButton>
                  </div>
                  {appBuilds.length === 0 ? (
                    <Empty compact icon={Hammer} title="No builds yet">
                      Build the app to produce its first image.
                    </Empty>
                  ) : (
                    appBuilds.map((build) => <BuildRow key={String(build.id ?? build.build_id)} build={build} customerId={customerId} workspaceId={workspaceId} runner={runner} call={call} />)
                  )}
                </>
              )}
            </div>
          )}

          {tab === "logs" &&
            (logs.isLoading ? (
              <Skeleton lines={2} />
            ) : (
              <Terminal title="Logs · last 200 lines" empty="No log lines yet." actions={<button type="button" className="button button-ghost" onClick={() => void logs.refetch()}>Refresh</button>}>
                {logs.isError || !Array.isArray(logs.data?.items)
                  ? "Logs are not available on this Lake yet."
                  : logs.data!.items!.map((item) => `${String(item.ts ?? "")} ${String(item.pod ?? "")}/${String(item.container ?? "")} ${String(item.line ?? "")}\n`).join("")}
              </Terminal>
            ))}

          {tab === "events" && (
            <div className="space-y-2">
              <Terminal
                title="Live events"
                live
                actions={
                  <Link to={streamHref(ws, customerId, `app-events.${appId}`)} className="text-inherit">
                    Browse app event stream
                  </Link>
                }
                empty="Waiting for app events…"
              >
                {events.map((event, index) => (
                  <div key={`${event.sequence}-${index}`}>{eventLine(event)}</div>
                ))}
              </Terminal>
            </div>
          )}

          {tab === "settings" && (
            <div className="space-y-4">
              <Card title="Environment" padded={false}>
                <EnvTable env={env} secretEnv={secretEnv} busy={runner.busy} call={call} />
                <div className="border-t border-hairline px-4 py-3">
                  <ResetForm
                    className="row-form grid grid-cols-1 items-end gap-2 sm:grid-cols-[1fr_1fr_1fr_1fr_1fr_auto]"
                    onSubmit={(form) => call("env", "set_app_env", envInput(form), `Set ${form.key}`)}
                  >
                    <label>
                      Key
                      <input name="key" required />
                    </label>
                    <label>
                      Value
                      <input name="value" />
                    </label>
                    <label>
                      Secret mount ID
                      <input name="mount_id" />
                    </label>
                    <label>
                      Secret name
                      <input name="secret_name" />
                    </label>
                    <label>
                      Field
                      <input name="field" />
                    </label>
                    <Button type="submit" loading={runner.busy === "env"}>
                      <Plus size={14} aria-hidden /> Set
                    </Button>
                  </ResetForm>
                  <p className="muted mb-0 mt-2 text-[12.5px]">
                    For a secret, name a field of a secret in a secret mount of this workspace instead of a value. Its value is never shown here; the app receives it when it is deployed, and again when the secret gets a new version.
                  </p>
                </div>
              </Card>
              <Card title="Authentication" label="Access">
                <form
                  className="stack"
                  onSubmit={submit("auth", (form) => call("auth", "set_app_auth", { auth: form.auth, confirm_public: form.confirm_public === "1" }, "Authentication changed"))}
                >
                  <label>
                    Access
                    <select name="auth" defaultValue={String(app.auth ?? "lake")} key={String(app.auth ?? "")}>
                      <option value="lake">Lake Sign-In</option>
                      <option value="none">Public</option>
                    </select>
                  </label>
                  <label>
                    <input type="checkbox" name="confirm_public" value="1" /> Confirm public access
                  </label>
                  <div>
                    <Button type="submit" loading={runner.busy === "auth"}>
                      Change authentication
                    </Button>
                  </div>
                </form>
              </Card>
            </div>
          )}

          {tab === "services" && (
            <div className="space-y-4">
              <Card
                label="Binding"
                title="Workspace Data"
                actions={
                  <ActionButton
                    busy={runner.busy}
                    id="zetta"
                    onClick={() => call("zetta", "set_app_zetta_binding", { attached: !hasZetta }, hasZetta ? "Workspace data detached" : "Workspace data attached")}
                  >
                    {hasZetta ? "Detach workspace data" : "Attach workspace data"}
                  </ActionButton>
                }
              >
                <Status value={hasZetta ? "Attached" : "Detached"} tone={hasZetta ? "success" : "neutral"} />
              </Card>
              <Card title="Services" padded={false}>
                {boundServices.length === 0 ? (
                  <p className="muted m-0 px-4 py-3 text-[13px]">No services attached.</p>
                ) : (
                  <ul className="m-0 list-none p-0">
                    {boundServices.map((serviceId) => {
                      const service = (services.data ?? []).find((s) => String(s.id) === serviceId);
                      return (
                        <li key={serviceId} className="flex flex-wrap items-center justify-between gap-2 border-b border-hairline px-4 py-2.5 last:border-b-0">
                          <div className="min-w-0">
                            <div className="font-semibold text-ink">{service ? String(service.name ?? "") : <code>{serviceId}</code>}</div>
                            <div className="truncate text-[12px] text-ink-3">{service ? `${String(service.kind ?? "")} · ${serviceId}` : "Service"}</div>
                          </div>
                          <ActionButton busy={runner.busy} id={`unbind-${serviceId}`} onClick={() => call(`unbind-${serviceId}`, "unbind_service_from_app", { service_id: serviceId }, "Service detached")}>
                            Detach
                          </ActionButton>
                        </li>
                      );
                    })}
                  </ul>
                )}
                <div className="border-t border-hairline px-4 py-3">
                  <form className="row-form flex flex-wrap items-end gap-2" onSubmit={submit("bind", (form) => call("bind", "bind_service_to_app", { service_id: form.service_id ?? "" }, "Service attached"))}>
                    <label className="min-w-0 flex-1">
                      Service
                      <select name="service_id">
                        {(services.data ?? []).map((service) => (
                          <option key={String(service.id)} value={String(service.id)}>
                            {String(service.name ?? "")} · {String(service.kind ?? "")}
                          </option>
                        ))}
                      </select>
                    </label>
                    <Button type="submit" loading={runner.busy === "bind"}>
                      Attach service
                    </Button>
                  </form>
                  {services.isError && <ErrorBlock error={services.error} retry={() => void services.refetch()} what="services" />}
                </div>
              </Card>
              <Card label="New service" title="Create and Attach">
                <ResetForm
                  className="row-form grid grid-cols-1 items-end gap-2 sm:grid-cols-[1fr_1fr_8rem_auto]"
                  onSubmit={(form) => {
                    const input: Row = { kind: form.kind, name: form.name };
                    if (/^\d+$/.test((form.size ?? "").trim())) input.storage_gi = Number(form.size);
                    return call("service", "create_service_and_bind_to_app", input, "Service created and attached");
                  }}
                >
                  <label>
                    Name
                    <input name="name" required />
                  </label>
                  <label>
                    Kind
                    <input name="kind" required />
                  </label>
                  <label>
                    Size
                    <input name="size" />
                  </label>
                  <Button type="submit" loading={runner.busy === "service"}>
                    Create and attach
                  </Button>
                </ResetForm>
              </Card>
            </div>
          )}
        </div>
      </Split>
    </Page>
  );
}

/** Deploys, rollbacks and the creation, newest first, each with Rollback to its target. */
function HistoryTable({ history, busy, call }: { history: AppEvent[]; busy: string | null; call: (key: string, capability: string, input: Row, done: string) => Promise<unknown> }) {
  const target = (event: AppEvent) => {
    const status = (event.status ?? {}) as Row;
    const eventSource = (status.source && typeof status.source === "object" ? status.source : {}) as Row;
    const image = typeof status.image === "string" ? status.image : "";
    const text = image || `${String(eventSource.repository ?? "")} @ ${String(eventSource.revision ?? "")}`;
    const rollback = image
      ? { image }
      : Object.keys(eventSource).length
        ? {
            source: {
              repository: String(eventSource.repository ?? ""),
              revision: String(eventSource.revision ?? ""),
              path: String(eventSource.path ?? ""),
              entrypoint: String(eventSource.entrypoint ?? ""),
            },
          }
        : null;
    return { text, rollback };
  };
  const columns: Column<AppEvent>[] = [
    {
      key: "action",
      label: "Action",
      render: (event) => (
        <div className="min-w-0">
          <div className="flex items-center gap-2">
            <Status value={String(event.type ?? "")} tone={event.type === "rollback" ? "warning" : "info"} />
            <span className="font-mono text-[12px] text-ink-3">#{String(event.sequence ?? "")}</span>
          </div>
          {event.message ? <div className="mt-0.5 text-[12px] text-ink-3">{String(event.message)}</div> : null}
        </div>
      ),
    },
    { key: "generation", label: "Generation", numeric: true, render: (event) => <span className="font-mono">{String(event.generation ?? "")}</span> },
    { key: "actor", label: "Actor", render: (event) => String(event.principal ?? "") },
    {
      key: "target",
      label: "Target",
      render: (event) => (
        <code className="block max-w-[14rem] truncate" title={target(event).text}>
          {target(event).text}
        </code>
      ),
    },
    {
      key: "rollback",
      label: "",
      render: (event) => {
        const { rollback, text } = target(event);
        const key = `rollback-${event.sequence}`;
        return rollback ? (
          <ConfirmButton
            title="Roll Back the App?"
            confirmLabel="Roll back"
            confirm={
              <>
                The app is deployed again at <code>{text}</code>.
              </>
            }
            onConfirm={() => call(key, "rollback_app", rollback, "Rollback requested")}
            disabled={busy === key}
          >
            <Undo2 size={14} aria-hidden /> Rollback
          </ConfirmButton>
        ) : null;
      },
    },
  ];
  if (history.length === 0) return <p className="muted m-0 px-4 py-3 text-[13px]">No deployments recorded yet.</p>;
  return <DataTable rows={history} columns={columns} rowKey={(event) => String(event.sequence)} caption="Deploy history" />;
}

/** The app's environment: plain values, then secret references masked. */
function EnvTable({
  env,
  secretEnv,
  busy,
  call,
}: {
  env: Record<string, unknown>;
  secretEnv: Record<string, unknown>;
  busy: string | null;
  call: (key: string, capability: string, input: Row, done: string) => Promise<unknown>;
}) {
  const rows = [
    ...Object.entries(env).map(([key, value]) => ({ key, value: typeof value === "string" ? value : JSON.stringify(value), secret: false })),
    ...Object.entries(secretEnv).map(([key, reference]) => ({ key, value: secretEnvLabel(reference), secret: true })),
  ];
  if (rows.length === 0) return <p className="muted m-0 px-4 py-3 text-[13px]">No environment variables set.</p>;
  return (
    <DataTable
      rows={rows}
      rowKey={(row) => `${row.secret ? "secret" : "env"}-${row.key}`}
      caption="Environment"
      columns={[
        { key: "key", label: "Key", sort: (row) => row.key, render: (row) => <code>{row.key}</code> },
        {
          key: "value",
          label: "Value",
          render: (row) => (
            <span className="flex items-center gap-2">
              <span className="break-all">{row.value}</span>
              {row.secret && <Status value="secret" tone="ai" />}
            </span>
          ),
        },
        {
          key: "unset",
          label: "",
          render: (row) => (
            <ActionButton busy={busy} id={`unset-${row.key}`} onClick={() => call(`unset-${row.key}`, "unset_app_env", { key: row.key }, `Unset ${row.key}`)}>
              Unset
            </ActionButton>
          ),
        },
      ]}
    />
  );
}

function ActionButton({ id, busy, onClick, children, primary }: { id: string; busy: string | null; onClick: () => unknown; children: ReactNode; primary?: boolean }) {
  return (
    <Button variant={primary ? "primary" : "secondary"} loading={busy === id} onClick={() => void onClick()}>
      {children}
    </Button>
  );
}

/** A form whose fields clear after its action succeeds. */
function ResetForm({ className, onSubmit, children }: { className?: string; onSubmit: (form: Record<string, string>) => Promise<unknown>; children: ReactNode }) {
  return (
    <form
      className={className}
      onSubmit={(event) => {
        event.preventDefault();
        const element = event.currentTarget;
        const form = formValues(event);
        void onSubmit(form).then((value) => {
          if (value !== undefined) element.reset();
        });
      }}
    >
      {children}
    </form>
  );
}

/** One build as a card: its status, the retained log (`build_logs`) and the live log after it
 * (`receive_build_events`, long poll) in a terminal, Cancel and Deploy build. */
function BuildRow({
  build,
  customerId,
  workspaceId,
  runner,
  call,
}: {
  build: Row;
  customerId: string;
  workspaceId: string | null;
  runner: ReturnType<typeof useRunner>;
  call: (key: string, capability: string, input: Row, done: string) => Promise<unknown>;
}) {
  const ws = useWorkspaceLink();
  const id = String(build.id ?? build.build_id ?? "");
  const image = String(build.image ?? "") || String(build.target ?? "");
  const logs = useRead<{ lines?: unknown[]; next_sequence?: number }>("build_logs", { customer_id: customerId, workspace_id: workspaceId ?? undefined, build_id: id });
  const retained = useMemo(() => (logs.data?.lines ?? []).map((line) => (typeof line === "string" ? line : JSON.stringify(line))).join("\n"), [logs.data]);
  const [live, setLive] = useState<string[]>([]);
  const start = logs.data?.next_sequence;
  useEffect(() => {
    if (!workspaceId || start === undefined) return;
    const controller = new AbortController();
    const signal = controller.signal;
    const key = `lake-build-events:${id}`;
    let next = Math.max(Number(start) || 1, storedNumber(key));
    // The retained log now holds everything before `start`.
    setLive([]);
    void (async () => {
      while (!signal.aborted) {
        try {
          const value = await read<{ events?: Row[]; next_sequence?: number }>(
            "receive_build_events",
            { customer_id: customerId, workspace_id: workspaceId, build_id: id, from_sequence: next, wait_ms: 25000 },
            { signal },
          );
          const lines = (value.events ?? []).map((event) => `${event.sequence ?? ""}  ${event.message || event.type || JSON.stringify(event)}`);
          if (lines.length) setLive((current) => [...current, ...lines]);
          next = Number(value.next_sequence) || next;
          storeNumber(key, next);
        } catch (error) {
          // An answer asking again cannot change (not set up, unavailable, refused) ends the
          // follow; anything else is retried after a pause (lib/errors.ts, lake#575).
          if (signal.aborted || stopsPolling(error)) break;
          await pause(2000, signal);
        }
      }
    })();
    return () => {
      controller.abort();
    };
  }, [customerId, workspaceId, id, start]);
  const cancelKey = `cancel-${id}`;
  const deployKey = `deploy-${id}`;
  const status = String(build.phase ?? build.status ?? "");
  return (
    <Card
      label={
        <span className="normal-case">
          <code>{id}</code>
        </span>
      }
      title={
        <span className="flex flex-wrap items-center gap-2">
          <Status value={status} /> <code className="text-[12.5px] font-normal">{String(build.revision ?? "")}</code>
        </span>
      }
      actions={
        <>
          {image && (
            <Button loading={runner.busy === deployKey} onClick={() => void call(deployKey, "deploy_app", { image }, "Deploy requested")}>
              Deploy build
            </Button>
          )}
          <Button
            variant="ghost"
            loading={runner.busy === cancelKey}
            onClick={() => void runner.run(cancelKey, () => act("cancel_build", { customer_id: customerId, workspace_id: workspaceId, build_id: id }), "Build cancellation requested")}
          >
            Cancel build
          </Button>
        </>
      }
      footer={<Link to={streamHref(ws, customerId, `build-events.${id}`)}>Browse build stream</Link>}
    >
      <details>
        <summary className="cursor-pointer text-[13px] text-ink-2">Logs</summary>
        <Terminal className="mt-2" title="Build log" live={live.length > 0 || undefined} empty="No log lines yet.">
          {logs.isError ? (logs.error as Error).message : [retained, ...live].filter(Boolean).join("\n")}
        </Terminal>
      </details>
    </Card>
  );
}
