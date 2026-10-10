/** Ship → Apps → one app (web_ui.cpp web_ui_app; handlers serve_app_page and the nineteen
 * serve_app_form actions): status and the rollout track, controls, deploy history with rollback,
 * live events and build logs by long poll, logs, builds, environment, bindings, authentication
 * and deletion. Every control calls its capability directly. */
import { type FormEvent, type ReactNode, useEffect, useMemo, useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { act, read, useList, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Badge, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { AppPhase, AppUrl, decimal, formValues } from "./AppsPage";
import { PageError, pause, qs, storedNumber, storeNumber, streamHref, useRunner, useWorkspaceLink } from "./shared";

type AppEvent = Row & { sequence?: number; type?: string; message?: string; status?: Row };

function eventLine(event: AppEvent): string {
  return `${event.sequence ?? ""}  ${event.type || "event"}  ${event.message || JSON.stringify(event.status ?? {})}`;
}

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
        } catch {
          if (signal.aborted) break;
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

  const submit = (_key: string, handler: (form: Record<string, string>) => Promise<unknown>) => (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = formValues(event);
    void handler(form);
  };

  return (
    <Page eyebrow="Ship · Apps" title={name} sub={<AppUrl url={app.url} />}>
      <PageError error={runner.error} />
      <section className="panel">
        <h2>Status</h2>
        <div className="app-phase">
          <span>Desired: {String(app.desired ?? "")}</span>
          <AppPhase phase={phase} ready={ready} />
          <span>
            Generation {String(app.generation ?? "")} / observed {String(app.observed_generation ?? "")}
          </span>
        </div>
        <div className="app-phase-track" aria-label="Rollout progress">
          <span className={ready ? "app-phase-fill ready" : "app-phase-fill"} />
        </div>
        <p>{String(app.message ?? "")}</p>
      </section>

      <section className="panel">
        <h2>Controls</h2>
        <div className="app-controls">
          <ActionButton busy={runner.busy} id="restart" onClick={() => call("restart", "restart_app", {}, "Restart requested")}>
            Restart
          </ActionButton>
          <ActionButton busy={runner.busy} id="stop" onClick={() => call("stop", "stop_app", {}, "Stop requested")}>
            Stop
          </ActionButton>
          <ActionButton busy={runner.busy} id="start" onClick={() => call("start", "start_app", {}, "Start requested")}>
            Start
          </ActionButton>
        </div>
        <form className="filters" onSubmit={submit("scale", (form) => call("scale", "scale_app", { replicas: decimal(form.replicas) }, "Scaled"))}>
          <label>
            Replicas
            <input type="number" name="replicas" min={1} max={100} defaultValue={String(app.replicas ?? "")} key={String(app.replicas ?? "")} />
          </label>
          <button type="submit" disabled={runner.busy === "scale"}>
            Scale
          </button>
        </form>
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
              Deploy Revision
              <input name="revision" defaultValue={String(source.revision ?? "")} key={String(source.revision ?? "")} required />
            </label>
          ) : (
            <label>
              Deploy Image
              <input name="image" defaultValue={String(app.image ?? "")} key={String(app.image ?? "")} required />
            </label>
          )}
          <button type="submit" disabled={runner.busy === "deploy"}>
            Deploy
          </button>
        </form>
      </section>

      <section className="panel">
        <h2>Deploy History</h2>
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Sequence</th>
                <th>Action</th>
                <th>Generation</th>
                <th>Actor</th>
                <th>Target</th>
                <th>Message</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {history.map((event) => {
                const status = (event.status ?? {}) as Row;
                const eventSource = (status.source && typeof status.source === "object" ? status.source : {}) as Row;
                const image = typeof status.image === "string" ? status.image : "";
                const target = image || `${String(eventSource.repository ?? "")} @ ${String(eventSource.revision ?? "")}`;
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
                const key = `rollback-${event.sequence}`;
                return (
                  <tr key={String(event.sequence)}>
                    <td>{String(event.sequence ?? "")}</td>
                    <td>{String(event.type ?? "")}</td>
                    <td>{String(event.generation ?? "")}</td>
                    <td>{String(event.principal ?? "")}</td>
                    <td>
                      <code>{target}</code>
                    </td>
                    <td>{String(event.message ?? "")}</td>
                    <td>
                      {rollback && (
                        <ActionButton busy={runner.busy} id={key} onClick={() => call(key, "rollback_app", rollback, "Rollback requested")}>
                          Rollback
                        </ActionButton>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        {history.length === 0 && <p className="muted">No deployments recorded yet.</p>}
      </section>

      <p>
        <Link to={streamHref(ws, customerId, `app-events.${appId}`)}>Browse app event stream</Link>
      </p>

      <section className="panel">
        <h2>Live Events</h2>
        <div className="app-stream" role="log">
          {events.map((event, index) => (
            <div key={`${event.sequence}-${index}`}>{eventLine(event)}</div>
          ))}
        </div>
      </section>

      <section className="panel">
        <h2>Logs</h2>
        {logs.isLoading ? (
          <Skeleton lines={2} />
        ) : (
          <pre className="app-stream">
            {logs.isError || !Array.isArray(logs.data?.items)
              ? "Logs are not available on this lake yet."
              : logs.data!.items!.map((item) => `${String(item.ts ?? "")} ${String(item.pod ?? "")}/${String(item.container ?? "")} ${String(item.line ?? "")}\n`).join("")}
          </pre>
        )}
      </section>

      <section className="panel">
        <h2>Builds</h2>
        {builds.isLoading ? (
          <Skeleton lines={2} />
        ) : builds.isError ? (
          <>
            <p className="muted">Builds are not available on this lake yet.</p>
            <button type="button" disabled>
              Build
            </button>
          </>
        ) : (
          <>
            {appBuilds.length === 0 ? (
              <p className="muted">No builds yet.</p>
            ) : (
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>Build</th>
                      <th>Status</th>
                      <th>Revision</th>
                      <th />
                    </tr>
                  </thead>
                  <tbody>
                    {appBuilds.map((build) => (
                      <BuildRow key={String(build.id ?? build.build_id)} build={build} customerId={customerId} workspaceId={workspaceId} runner={runner} call={call} />
                    ))}
                  </tbody>
                </table>
              </div>
            )}
            <ActionButton busy={runner.busy} id="build" onClick={() => runner.run("build", () => act("build_app", { customer_id: customerId, workspace_id: workspaceId, app: appId }), "Build queued")}>
              {appBuilds.length === 0 ? "Build" : "Rebuild"}
            </ActionButton>
          </>
        )}
      </section>

      <section className="panel">
        <h2>Environment</h2>
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Key</th>
                <th>Value</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {Object.entries(env).map(([key, value]) => (
                <tr key={`env-${key}`}>
                  <td>
                    <code>{key}</code>
                  </td>
                  <td>{typeof value === "string" ? value : JSON.stringify(value)}</td>
                  <td>
                    <ActionButton busy={runner.busy} id={`unset-${key}`} onClick={() => call(`unset-${key}`, "unset_app_env", { key }, `Unset ${key}`)}>
                      Unset
                    </ActionButton>
                  </td>
                </tr>
              ))}
              {Object.keys(secretEnv).map((key) => (
                <tr key={`secret-${key}`}>
                  <td>
                    <code>{key}</code>
                  </td>
                  <td>••••••</td>
                  <td>
                    <ActionButton busy={runner.busy} id={`unset-${key}`} onClick={() => call(`unset-${key}`, "unset_app_env", { key }, `Unset ${key}`)}>
                      Unset
                    </ActionButton>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <ResetForm
          className="filters"
          onSubmit={(form) => call("env", "set_app_env", form.secret_ref ? { key: form.key, secret_ref: form.secret_ref } : { key: form.key, value: form.value ?? "" }, `Set ${form.key}`)}
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
            Secret Reference
            <input name="secret_ref" />
          </label>
          <button type="submit" disabled={runner.busy === "env"}>
            Set Environment
          </button>
        </ResetForm>
      </section>

      <section className="panel">
        <h2>Bindings</h2>
        <p>
          Workspace Zetta: <Badge tone={hasZetta ? "success" : "neutral"}>{hasZetta ? "Attached" : "Detached"}</Badge>
        </p>
        <ActionButton
          busy={runner.busy}
          id="zetta"
          onClick={() => call("zetta", "set_app_zetta_binding", { attached: !hasZetta }, hasZetta ? "Zetta detached" : "Zetta attached")}
        >
          {hasZetta ? "Detach Zetta" : "Attach Zetta"}
        </ActionButton>
        {boundServices.map((serviceId) => (
          <p key={serviceId}>
            <code>{serviceId}</code>{" "}
            <ActionButton busy={runner.busy} id={`unbind-${serviceId}`} onClick={() => call(`unbind-${serviceId}`, "unbind_service_from_app", { service_id: serviceId }, "Service detached")}>
              Detach
            </ActionButton>
          </p>
        ))}
        <form className="filters" onSubmit={submit("bind", (form) => call("bind", "bind_service_to_app", { service_id: form.service_id ?? "" }, "Service attached"))}>
          <label>
            Service
            <select name="service_id">
              {(services.data ?? []).map((service) => (
                <option key={String(service.id)} value={String(service.id)}>
                  {String(service.name ?? "")} · {String(service.kind ?? "")}
                </option>
              ))}
            </select>
          </label>
          <button type="submit" disabled={runner.busy === "bind"}>
            Attach Service
          </button>
        </form>
        {services.isError && <ErrorBlock error={services.error} retry={() => void services.refetch()} what="services" />}
        <ResetForm
          className="filters"
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
          <button type="submit" disabled={runner.busy === "service"}>
            Create and Attach
          </button>
        </ResetForm>
      </section>

      <section className="panel">
        <h2>Authentication</h2>
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
          <button type="submit" disabled={runner.busy === "auth"}>
            Change Authentication
          </button>
        </form>
      </section>

      <section className="panel danger">
        <h2>Danger Zone</h2>
        <form className="stack" onSubmit={submit("delete", (form) => call("delete", "delete_app", { confirm: form.confirm ?? "" }, `Deletion of ${name} requested`))}>
          <label>
            Type <code>{name}</code> to delete
            <input name="confirm" required />
          </label>
          <button type="submit" disabled={runner.busy === "delete"}>
            Delete App
          </button>
        </form>
      </section>
    </Page>
  );
}

function ActionButton({ id, busy, onClick, children }: { id: string; busy: string | null; onClick: () => unknown; children: ReactNode }) {
  return (
    <button type="button" disabled={busy === id} onClick={() => void onClick()}>
      {children}
    </button>
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

/** One build: its status, the retained log (`build_logs`) and the live log after it
 * (`receive_build_events`, long poll), Cancel and Deploy Build. */
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
        } catch {
          if (signal.aborted) break;
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
  return (
    <tr>
      <td>
        <code>{id}</code>
      </td>
      <td>{String(build.phase ?? build.status ?? "")}</td>
      <td>
        <code>{String(build.revision ?? "")}</code>
      </td>
      <td>
        <button
          type="button"
          disabled={runner.busy === cancelKey}
          onClick={() => void runner.run(cancelKey, () => act("cancel_build", { customer_id: customerId, workspace_id: workspaceId, build_id: id }), "Build cancellation requested")}
        >
          Cancel Build
        </button>{" "}
        <Link to={streamHref(ws, customerId, `build-events.${id}`)}>Browse build stream</Link>{" "}
        {image && (
          <button type="button" disabled={runner.busy === deployKey} onClick={() => void call(deployKey, "deploy_app", { image }, "Deploy requested")}>
            Deploy Build
          </button>
        )}
        <details>
          <summary>Logs</summary>
          <pre>{logs.isError ? (logs.error as Error).message : retained}</pre>
          <div className="app-stream">
            {live.map((line, index) => (
              <div key={index}>{line}</div>
            ))}
          </div>
        </details>
      </td>
    </tr>
  );
}
