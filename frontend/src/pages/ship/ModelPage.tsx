/** Ship → Model (`/ship/model?id=`, lake#599): serving one model version.
 * - Inference: publish or unpublish, the endpoint, the call to paste, and a try-it panel that
 *   calls `predict` with a row. A model a workspace methodology trained (`python:W-…`) is served
 *   by its own code (lake#690): publishing starts its server, whose state the side card follows
 *   (`get_methodology_deployment`), and `predict` answers one prediction per row.
 * - Access: the keys that may call it (tokens scoped to `predict`, and a new one), and origins.
 * - Destinations: where its answers go.
 * - Monitoring: its calls, errors and latency from the workspace's own series (`model_traffic`),
 *   and drift, which is not measured yet.
 * Training it is Train → Model, linked from the header. */
import { useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Activity, AlertTriangle, Gauge, GraduationCap, KeyRound, Play, Plus, Trash2 } from "lucide-react";
import { act, useList, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { modelLines } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, CopyBlock, DataTable, Empty, ErrorBlock, Field, FormError, KeyValue, Notice, Skeleton, Split, Stat, StatRow, StateTabs, Status, fieldError, Form } from "@/lib/ui";
import { NO_WORKSPACE } from "@/pages/account/signIn";
import { Chart } from "../experiments/Chart";
import { WorkspaceFrame, cellText, qs, text, useSpaceRead, useWorkspace, useWsPath, wa } from "../train/workspace";
import { errorRate, featuresOf, filledSteps, groupOf, methodologyOf, parseRows, predictCurl, predictionRows, predictKeys, sampleRow, serverState, trafficOf, versionNumber, type ModelGroup } from "../train/models";
import { SourceText, VersionChip, useModelGroups, useModelLinks } from "../train/modelParts";
import { CopyMono, useOrigin } from "./parts";
import { UsageThisMonth } from "../usage/UsageThisMonth";

type View = "inference" | "access" | "destinations" | "monitoring";
const VIEWS: View[] = ["inference", "access", "destinations", "monitoring"];

export default function ShipModelPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const links = useModelLinks();
  const navigate = useNavigate();
  const { customerId, workspaceId } = useWorkspace();
  const { q, groups } = useModelGroups();
  const group = groupOf(groups, id);
  const version = group?.versions.find((v) => text(v, "id") === id);
  useTrail([{ label: "Ship", to: links.shipList }, { label: "Models", to: links.shipList }, entityCrumb(group?.name ?? id)]);
  return (
    <WorkspaceFrame
      eyebrow="Ship"
      title={
        group && version ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {group.name} <VersionChip n={versionNumber(group, id)} />
            {version.exposed === true ? <Status value="published" tone="success" /> : <Status value="not published" tone="neutral" />}
          </span>
        ) : (
          "Model"
        )
      }
      sub="Serving this model: how to call it, who may, where its answers go, and how it is doing."
      actions={
        id ? (
          <>
            <Button to={links.train(id)}>
              <GraduationCap size={14} aria-hidden /> Training
            </Button>
            {group && version ? (
              <DeletionButton
                capability="delete_model"
                input={{ customer_id: customerId, workspace_id: workspaceId, id }}
                title={`Delete ${group.name} v${versionNumber(group, id)}`}
                confirmLabel="Delete model version"
                intro="Deleting this version removes the trained model from the workspace. It cannot be undone; its other versions stay."
                typed={() => group.name}
                lines={modelLines}
                done={`Deleted ${group.name} v${versionNumber(group, id)}`}
                onDone={() => navigate(links.shipList)}
              >
                <Trash2 size={14} aria-hidden /> Delete
              </DeletionButton>
            ) : null}
          </>
        ) : null
      }
    >
      {() => {
        if (q.isLoading) return <Skeleton lines={6} />;
        if (q.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what="the models" />;
        if (!group || !version)
          return (
            <Empty title="No such model" action={<Button to={links.shipList}>Every model</Button>}>
              This workspace has no model version <code>{id || "(none named)"}</code>.
            </Empty>
          );
        return <Serving key={id} group={group} version={version} />;
      }}
    </WorkspaceFrame>
  );
}

function Serving({ group, version }: { group: ModelGroup; version: Row }) {
  const [params, setParams] = useSearchParams();
  const view = (VIEWS as string[]).includes(params.get("view") ?? "") ? (params.get("view") as View) : "inference";
  const setView = (next: View) => {
    const changed = new URLSearchParams(params);
    if (next === "inference") changed.delete("view");
    else changed.set("view", next);
    setParams(changed, { replace: true });
  };
  const links = useModelLinks();
  const id = text(version, "id");
  const live = group.published.find((v) => v !== version);
  return (
    <Split side={<Side group={group} version={version} />}>
      {version.exposed !== true && live && (
        <Banner tone="info" title={`v${versionNumber(group, text(live, "id"))} Is the Version Serving`} action={<Button to={links.ship(text(live, "id"))}>Open v{versionNumber(group, text(live, "id"))}</Button>}>
          This page is v{versionNumber(group, id)}, which is not published.
        </Banner>
      )}
      <StateTabs<View>
        value={view}
        onChange={setView}
        label="Serving"
        tabs={[
          { value: "inference", label: "Inference" },
          { value: "access", label: "Access" },
          { value: "destinations", label: "Destinations" },
          { value: "monitoring", label: "Monitoring" },
        ]}
      />
      {view === "inference" && <Inference version={version} />}
      {view === "access" && <Access group={group} />}
      {view === "destinations" && <Destinations />}
      {view === "monitoring" && <Monitoring version={version} />}
    </Split>
  );
}

/* ------------------------------------------------------------------ the side column */

function Side({ group, version }: { group: ModelGroup; version: Row }) {
  const scope = useWorkspace();
  const links = useModelLinks();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const id = text(version, "id");
  const n = versionNumber(group, id);
  const on = version.exposed === true;
  const methodology = methodologyOf(version);
  const server = useServer(id, methodology !== "");
  const wsPath = useWsPath();
  const publish = async () => {
    setBusy(true);
    setError(null);
    try {
      await wa(scope, "expose_model", { model_id: id });
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <>
      <UsageThisMonth
        customerId={scope.customerId}
        workspaceId={scope.workspaceId}
        match={[{ dimension: "predictions", breakdown: "by_object", keys: [id], label: "Predictions answered" }]}
      />
      <Card label="Serving" title={on ? `v${n} answers outside calls` : `v${n} is not published`}>
        <div className="space-y-2">
          {on ? (
            <ConfirmButton
              title={`Unpublish ${group.name} v${n}?`}
              confirm="Unpublishing a model stops every outside call at once. You can publish it again later."
              confirmLabel="Unpublish"
              onConfirm={() => wa(scope, "unexpose_model", { model_id: id })}
            >
              Unpublish
            </ConfirmButton>
          ) : (
            <Button variant="primary" loading={busy} onClick={() => void publish()}>
              Publish v{n}
            </Button>
          )}
          <FormError error={error} />
          {methodology && (
            <div className="space-y-1" data-testid="methodology-server">
              <p className="m-0 text-[12.5px]">
                Served by its own code, the methodology <Link to={wsPath(`/design/methodology?${qs({ id: methodology })}`)}>{methodology}</Link>, in its sandbox.{" "}
                <Status value={server.state.label} tone={server.state.tone} />
              </p>
              {server.message && <p className="muted m-0 text-[12.5px]">{server.message}</p>}
            </div>
          )}
          {group.versions[0] !== version && (
            <p className="muted m-0 text-[12.5px]">
              v{versionNumber(group, text(group.latest, "id"))} is newer. <Link to={links.train(text(group.latest, "id"))}>Compare and promote it</Link> in Train.
            </p>
          )}
        </div>
      </Card>
      <Card label="Facts">
        <KeyValue
          rows={[
            ["Model id", <CopyMono key="id" value={id} short={12} label="Copy the model id" />],
            ["Version", <span key="v">v{n} of {group.versions.length}</span>],
            ["Source", <SourceText key="s" group={group} />],
            ["Trained", relative(text(version, "created"), Date.now(), "—")],
            ["Features", <span key="f">{featuresOf(version).length || "—"}</span>],
            ["Training", <Link key="t" to={links.train(id)}>Train → Models</Link>],
          ]}
        />
      </Card>
    </>
  );
}

/* ------------------------------------------------------------------ inference */

function Inference({ version }: { version: Row }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = useOrigin();
  const id = text(version, "id");
  const on = version.exposed === true;
  const [rows, setRows] = useState(() => JSON.stringify([sampleRow(version)], null, 2));
  const [answer, setAnswer] = useState<Row | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const run = async () => {
    const parsed = parseRows(rows);
    if ("error" in parsed) {
      setError(new Error(parsed.error));
      return;
    }
    setBusy(true);
    setError(null);
    try {
      setAnswer(await act<Row>("predict", { customer_id: customerId, workspace_id: workspaceId, model_id: id, rows: parsed.rows }));
    } catch (e) {
      setAnswer(null);
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const ranked = Array.isArray(answer?.ranked) ? (answer!.ranked as Row[]) : [];
  const predictions = predictionRows(answer);
  const methodology = methodologyOf(version);
  const server = useServer(id, methodology !== "");
  const callable = on && (!methodology || server.state.ready);
  return (
    <div className="space-y-4">
      <Card label="Endpoint">
        <KeyValue
          rows={[
            ["REST", <code key="r">POST {origin}/v1/predict</code>],
            ["MCP", <span key="m">the tool <code>predict</code> at <code>{origin}/mcp</code></span>],
            ["model_id", <CopyMono key="id" value={id} label="Copy the model id" />],
          ]}
        />
        {!on && (
          <div className="mt-3">
            <Notice tone="warning">This version is not published, so <code>predict</code> refuses it. Publish it to call it.</Notice>
          </div>
        )}
        {on && methodology && !server.state.ready && (
          <div className="mt-3">
            <Notice tone={server.state.tone === "danger" ? "danger" : "info"}>
              Its server is {server.state.label}{server.message ? `: ${server.message}` : "."} <code>predict</code> answers once it is serving.
            </Notice>
          </div>
        )}
      </Card>
      <CopyBlock label="Call it from your code" value={predictCurl(origin, customerId, workspaceId, version)} />
      <Card label="Try it" title="Score a Row" actions={<Button variant="primary" loading={busy} disabled={!callable} onClick={() => void run()}><Play size={14} aria-hidden /> Predict</Button>}>
        <Field label="Rows, as JSON" help="One object or a list of them, keyed by the model's features; `entity` is each row's own key.">
          <textarea className="font-mono text-[12.5px]" rows={8} value={rows} onChange={(e) => setRows(e.target.value)} spellCheck={false} />
        </Field>
        <FormError error={error} />
        {answer && (
          <div className="mt-3">
            {predictions.length ? (
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th scope="col">Row</th>
                      <th scope="col">Entity</th>
                      <th scope="col" className="text-right">Prediction</th>
                    </tr>
                  </thead>
                  <tbody>
                    {predictions.map((entry, i) => (
                      <tr key={i}>
                        <td className="font-mono">{cellText(entry.row)}</td>
                        <td className="font-mono">{cellText(entry.entity)}</td>
                        <td className="text-right font-mono">{typeof entry.prediction === "number" ? entry.prediction.toFixed(3) : cellText(entry.prediction)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : ranked.length ? (
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th scope="col">Row</th>
                      <th scope="col">Entity</th>
                      <th scope="col" className="text-right">Risk</th>
                    </tr>
                  </thead>
                  <tbody>
                    {ranked.map((entry, i) => (
                      <tr key={i}>
                        <td className="font-mono">{cellText(entry.row)}</td>
                        <td className="font-mono">{cellText(entry.entity)}</td>
                        <td className="text-right font-mono">{typeof entry.risk === "number" ? entry.risk.toFixed(3) : cellText(entry.risk)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : (
              <p className="muted m-0">The model answered no ranked rows.</p>
            )}
            <details className="mt-2">
              <summary>The whole answer</summary>
              <pre>{JSON.stringify(answer, null, 2)}</pre>
            </details>
          </div>
        )}
      </Card>
    </div>
  );
}

/* ------------------------------------------------------------------ its own server (lake#690) */

/** A methodology model's server: refreshed while it is on its way, still once it is settled. */
function useServer(modelId: string, enabled: boolean) {
  const q = useSpaceRead<{ deployment?: Row | null }>(
    enabled ? "get_methodology_deployment" : null,
    { model_id: modelId },
    { refetchInterval: (query) => (serverState((query.state.data as { deployment?: Row | null } | undefined)?.deployment).settled ? false : 4000) },
  );
  const deployment = q.data?.deployment ?? null;
  return { state: serverState(deployment), message: deployment ? text(deployment, "message") : "" };
}

/* ------------------------------------------------------------------ ingress */

function Access({ group }: { group: ModelGroup }) {
  const sessions = useList<Row>("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const keys = predictKeys(sessions.data);
  const [creating, setCreating] = useState(false);
  const [secret, setSecret] = useState<string | null>(null);
  return (
    <div className="space-y-4">
      <Card
        label="Who may call it"
        title="Keys Scoped to predict"
        actions={
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New predict key
          </Button>
        }
      >
        <p className="muted mt-0 text-[12.5px]">
          A call needs a Lake key of someone with read on this workspace. A key scoped to <code>predict</code> can call any published model that person can read, and nothing else. These are your keys; each person makes their own.
        </p>
        {secret && (
          <div className="mb-3">
            <Banner tone="ok" title="Copy the Key Now">
              It is shown once.
            </Banner>
            <CopyBlock value={secret} />
          </div>
        )}
        <DataTable<Row>
          q={sessions}
          rows={keys}
          what="keys"
          rowKey={(row, i) => text(row, "id") || String(i)}
          empty={
            <Empty compact icon={KeyRound} title="No predict key yet">
              Make one to call this model from your code.
            </Empty>
          }
          columns={[
            { key: "name", label: "Key", render: (row) => <strong>{text(row, "name") || "unnamed"}</strong> },
            {
              key: "scopes",
              label: "Scopes",
              render: (row) => (
                <div className="flex flex-wrap gap-1">
                  {((row.scopes ?? []) as string[]).map((scope) => (
                    <Chip key={scope}>{scope}</Chip>
                  ))}
                </div>
              ),
            },
            { key: "expires", label: "Expires", render: (row) => (text(row, "expires_at") ? relative(text(row, "expires_at"), Date.now(), "—") : <span className="muted">never</span>) },
          ]}
        />
        <p className="muted mb-0 mt-3 text-[12.5px]">
          Revoke a key on <Link to="/tokens">Account → API Tokens</Link>.
        </p>
      </Card>
      <Card label="Allowed origins">
        <p className="m-0 text-[13px]">
          A call carrying a key is answered from any origin. A browser signed in to Lake is refused when the page calling is not Lake's own. Lake keeps no per-model list of origins.
        </p>
      </Card>
      {creating && (
        <NewPredictKey
          name={`predict ${group.name}`}
          onClose={() => setCreating(false)}
          onCreated={(value) => {
            setSecret(value);
            setCreating(false);
          }}
        />
      )}
    </div>
  );
}

function NewPredictKey({ name: initial, onClose, onCreated }: { name: string; onClose: () => void; onCreated: (secret: string | null) => void }) {
  const toast = useToast();
  const [name, setName] = useState(initial);
  const [expiresIn, setExpiresIn] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const create = async () => {
    setBusy(true);
    setError(null);
    try {
      const seconds = expiresIn.trim() ? Number.parseInt(expiresIn, 10) : null;
      const value = await act<{ secret?: string }>(
        "create_token",
        { name, scopes: ["predict"], static: false, ...(seconds !== null && Number.isFinite(seconds) ? { expires_in_seconds: seconds } : {}) },
        NO_WORKSPACE,
      );
      toast("Predict key created");
      onCreated(value.secret ?? null);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title="New Predict Key"
      subtitle="A key that can call predict and nothing else. Its secret is shown once."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-predict-key" variant="primary" loading={busy}>
            Create key
          </Button>
        </>
      }
    >
      <Form id="new-predict-key" onSubmit={create}>
        <FormError error={fieldError(error, "name") || fieldError(error, "expires_in_seconds") ? null : error} />
        <Field label="Name" error={fieldError(error, "name")} required>
          <input type="text" name="name" value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
        <Field label="Expiry in seconds" error={fieldError(error, "expires_in_seconds")} help="Leave empty for as long as this session allows, at most 30 days.">
          <input type="number" name="expires_in_seconds" min={1} value={expiresIn} onChange={(e) => setExpiresIn(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

/* ------------------------------------------------------------------ egress */

function Destinations() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  return (
    <div className="space-y-4">
      <Card label="Where its answers go">
        <p className="m-0 text-[13px]">Each answer goes back to the caller that asked, and nowhere else. Lake keeps no copy of the rows sent or the scores answered.</p>
      </Card>
      <Card label="Apps" actions={<Button variant="ghost" to={path(`/ship/apps?${qs({ customer_id: customerId })}`)}>Ship → Apps</Button>}>
        <p className="m-0 text-[13px]">No app is bound to a model. An app calls this model the way any code does: <code>predict</code> with a key in its environment.</p>
      </Card>
      <Card label="Topics and webhooks">
        <p className="m-0 text-[13px]">Lake does not send a model's answers to a topic or a webhook yet.</p>
      </Card>
    </div>
  );
}

/* ------------------------------------------------------------------ monitoring */

const RANGES = [
  { id: "1h", label: "1 hour", ms: 3_600_000 },
  { id: "24h", label: "24 hours", ms: 86_400_000 },
  { id: "7d", label: "7 days", ms: 7 * 86_400_000 },
  { id: "30d", label: "30 days", ms: 30 * 86_400_000 },
] as const;

function Monitoring({ version }: { version: Row }) {
  const id = text(version, "id");
  const [range, setRange] = useState<(typeof RANGES)[number]["id"]>("24h");
  const span = RANGES.find((r) => r.id === range)!.ms;
  // A whole minute, so the read is the same for a minute and the steps line up.
  const [now] = useState(() => Date.now() - (Date.now() % 60_000) + 60_000);
  const traffic = useSpaceRead<Row>("model_traffic", { model_id: id, from_ms: now - span, to_ms: now }, { refetchInterval: 60_000 });
  const t = trafficOf(traffic.data, id);
  const fromMs = Number(traffic.data?.from_ms ?? now - span);
  const toMs = Number(traffic.data?.to_ms ?? now);
  const stepMs = Number(traffic.data?.step_ms ?? 0);
  const steps = useMemo(() => filledSteps(t, fromMs, toMs, stepMs), [t, fromMs, toMs, stepMs]);
  const callsData = useMemo(() => [steps.map((s) => s.at / 1000), steps.map((s) => s.calls), steps.map((s) => s.errors)], [steps]);
  const latencyData = useMemo(() => [steps.map((s) => s.at / 1000), steps.map((s) => s.meanMs)], [steps]);
  const callLabels = useMemo(() => ["Calls", "Errors"], []);
  const latencyLabels = useMemo(() => ["Mean latency (ms)"], []);
  const recent = [...t.steps].filter((s) => s.calls > 0).reverse().slice(0, 20);
  const requests = (traffic.data?.requests ?? {}) as Row;
  return (
    <div className="space-y-4">
      <div className="flex flex-wrap items-center gap-1.5">
        {RANGES.map((r) => (
          <Chip key={r.id} active={range === r.id} onClick={() => setRange(r.id)}>
            {r.label}
          </Chip>
        ))}
      </div>
      {traffic.isError ? (
        <ErrorBlock error={traffic.error} what="the traffic" retry={() => void traffic.refetch()} />
      ) : traffic.isLoading ? (
        <Skeleton lines={4} />
      ) : (
        <>
          <StatRow>
            <Stat label="Calls" value={t.calls} icon={Activity} sub={`of this version, last ${RANGES.find((r) => r.id === range)!.label}`} />
            <Stat label="Errors" value={t.errors} icon={AlertTriangle} tone={t.errors ? "bad" : undefined} sub={t.calls ? `${errorRate(t)} · ${t.clientErrors} refused, ${t.serverErrors} failed` : "no calls"} />
            <Stat label="Mean latency" value={t.meanMs === null ? "—" : `${t.meanMs.toFixed(0)} ms`} icon={Gauge} sub="inside Lake, per call" />
            <Stat label="Workspace predict requests" value={cellText(requests.total ?? 0)} sub="every model, as Lake counted them" />
          </StatRow>
          <Card label="Calls and errors">
            {t.calls ? <Chart chartKey={`calls-${id}-${range}`} data={callsData} labels={callLabels} settings={{ title: "Calls and errors", xLabel: "Time", time: true }} /> : <p className="muted m-0 text-[13px]">No calls in this range.</p>}
          </Card>
          <Card label="Latency">
            {t.meanMs !== null ? <Chart chartKey={`latency-${id}-${range}`} data={latencyData} labels={latencyLabels} settings={{ title: "Mean latency", xLabel: "Time", time: true }} /> : <p className="muted m-0 text-[13px]">No calls in this range.</p>}
          </Card>
          <Card label="Recent calls" padded={false}>
            <DataTable<(typeof recent)[number]>
              rows={recent}
              what="recent calls"
              rowKey={(s) => String(s.at)}
              empty={
                <div className="p-4">
                  <Empty compact title="No calls yet">
                    Calls appear here within a minute of being made.
                  </Empty>
                </div>
              }
              columns={[
                { key: "at", label: stepMs > 60_000 ? `Starting (${Math.round(stepMs / 60_000)} min)` : "Minute", render: (s) => new Date(s.at).toLocaleString() },
                { key: "calls", label: "Calls", numeric: true, render: (s) => <span className="font-mono">{s.calls}</span> },
                { key: "errors", label: "Errors", numeric: true, render: (s) => <span className={s.errors ? "font-mono text-bad" : "font-mono"}>{s.errors}</span> },
                { key: "mean", label: "Mean latency", numeric: true, render: (s) => (s.meanMs === null ? "—" : <span className="font-mono">{s.meanMs.toFixed(0)} ms</span>) },
              ]}
            />
          </Card>
          <Banner tone="quiet" title="Prediction Drift Is Not Measured Yet">
            {capitalized(text((traffic.data?.drift ?? {}) as Row, "why") || "the workspace keeps no reference distribution of the training data")}. Lake counts calls by the minute; it keeps no row a caller sent.
          </Banner>
        </>
      )}
    </div>
  );
}

function capitalized(value: string): string {
  return value ? value[0].toUpperCase() + value.slice(1) : value;
}
