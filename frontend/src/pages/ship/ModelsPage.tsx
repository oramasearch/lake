/** Ship → Models (lake#599): serving the workspace's models. Each model with the version that
 * answers outside calls, its calls, errors and latency over the last day (`model_traffic`, from the
 * workspace's own series), and Publish, Unpublish and Call. A row opens the model's serving page;
 * how it was trained is Train → Models. Publish and Unpublish call `expose_model` /
 * `unexpose_model` in the workspace. */
import { useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { Activity, AlertTriangle, Brain, Code, Gauge, Globe, GraduationCap } from "lucide-react";
import type { Row } from "@/lib/api";
import { Drawer, ConfirmButton } from "@/lib/overlay";
import { useTrail } from "@/lib/trail";
import { Button, Chip, CopyBlock, DataTable, Empty, ErrorBlock, FilterChips, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, text, useSpaceRead, useWorkspace, useWsPath, wa } from "../train/workspace";
import { errorRate, predictCurl, trafficOf, versionNumber, type ModelGroup } from "../train/models";
import { SourceText, VersionChip, useModelGroups, useModelLinks } from "../train/modelParts";
import { CopyMono, useOrigin } from "./parts";

export default function ModelsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const links = useModelLinks();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Ship", to: path(`/ship/models?${space}`) },
    { label: "Models", to: path(`/ship/models?${space}`) },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Ship"
      title="Models"
      sub="Serving your models: which version answers outside calls, who may call it, and how it is doing."
      actions={
        <Button to={links.trainList}>
          <GraduationCap size={14} aria-hidden /> Train → Models
        </Button>
      }
    >
      {() => <Models />}
    </WorkspaceFrame>
  );
}

type Filter = "published" | "unpublished";

/** The version a model serves: its published one, else its latest. */
export function servedVersion(group: ModelGroup): Row {
  return group.published[0] ?? group.latest;
}

function Models() {
  const scope = useWorkspace();
  const links = useModelLinks();
  const { q, groups, experiments } = useModelGroups();
  const traffic = useSpaceRead<Row>("model_traffic", {}, { refetchInterval: 60_000 });
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [needle, setNeedle] = useState("");
  const [state, setState] = useState<Filter | null>(null);
  /** The Drawer with the call snippets: one model's, or every published model's (""). */
  const [calling, setCalling] = useState<string | null>(null);

  const publish = async (id: string, published: boolean) => {
    setBusy(id);
    setError(null);
    try {
      await wa(scope, published ? "unexpose_model" : "expose_model", { model_id: id });
    } catch (e) {
      setError(e);
    } finally {
      setBusy(null);
    }
  };

  const published = useMemo(() => groups.filter((group) => group.published.length), [groups]);
  const day = useMemo(() => {
    let calls = 0;
    let errors = 0;
    let weighted = 0;
    for (const entry of (traffic.data?.models ?? []) as Row[]) {
      const t = trafficOf(traffic.data, text(entry, "model_id"));
      calls += t.calls;
      errors += t.errors;
      weighted += (t.meanMs ?? 0) * t.calls;
    }
    return { calls, errors, meanMs: calls ? weighted / calls : null };
  }, [traffic.data]);
  const lower = needle.trim().toLowerCase();
  const rows = groups.filter(
    (group) =>
      (!lower || [group.name, group.kind, group.pipeline, group.projectName, ...group.versions.map((v) => text(v, "id"))].join(" ").toLowerCase().includes(lower)) &&
      (!state || (state === "published") === group.published.length > 0),
  );
  const allPublished = published.map((group) => group.published[0]);

  return (
    <>
      {error ? <ErrorBlock error={error} /> : null}
      {q.data && groups.length > 0 && (
        <>
          <StatRow>
            <Stat label="Published" value={published.length} icon={Globe} tone={published.length ? "ok" : undefined} sub={`of ${groups.length} ${groups.length === 1 ? "model" : "models"}`} />
            <Stat label="Calls, 24 hours" value={traffic.data ? day.calls : "—"} icon={Activity} sub="predict calls of every model" />
            <Stat label="Errors, 24 hours" value={traffic.data ? day.errors : "—"} icon={AlertTriangle} tone={day.errors ? "bad" : undefined} sub={day.calls ? `${((100 * day.errors) / day.calls).toFixed(1)}% of calls` : "no calls"} />
            <Stat label="Mean latency" value={day.meanMs === null ? "—" : `${day.meanMs.toFixed(0)} ms`} icon={Gauge} sub="inside Lake, per call" />
          </StatRow>
          {traffic.isError ? <ErrorBlock error={traffic.error} what="the traffic" retry={() => void traffic.refetch()} /> : null}
          <Toolbar>
            <SearchInput value={needle} onChange={setNeedle} placeholder="Search models…" label="Search models" />
            <FilterChips<Filter>
              options={[
                { id: "published", label: "Published", count: published.length },
                { id: "unpublished", label: "Not published", count: groups.length - published.length },
              ]}
              value={state}
              onChange={setState}
            />
            <div className="ml-auto">
              <Button onClick={() => setCalling("")}>
                <Code size={14} aria-hidden /> Call a model
              </Button>
            </div>
          </Toolbar>
        </>
      )}
      <DataTable<ModelGroup>
        q={q}
        what="models"
        caption="Models"
        rows={rows}
        rowKey={(group) => group.key}
        initialSort={{ key: "calls", dir: "descending" }}
        empty={
          groups.length === 0 ? (
            <Empty icon={Brain} title="No models yet" action={<Button to={links.trainList}>Go to Train → Models</Button>}>
              Models appear here when a run trains one. Publish one to call it from outside Lake.
            </Empty>
          ) : (
            <Empty compact title="No model matches">
              Clear the search or a filter to see more.
            </Empty>
          )
        }
        columns={[
          {
            key: "model",
            label: "Model",
            sort: (group) => group.name.toLowerCase(),
            render: (group) => {
              const served = servedVersion(group);
              return (
                <div className="min-w-0">
                  <div className="flex flex-wrap items-center gap-1.5">
                    <Link className="font-semibold" to={links.ship(text(served, "id"))}>
                      {group.name}
                    </Link>
                    {group.kind && <Chip>{group.kind.replace(/[_-]+/g, " ")}</Chip>}
                    {group.versions.some((v) => experiments.promoted.has(text(v, "id"))) && <Chip tone="ok">promoted</Chip>}
                  </div>
                  <small className="muted">
                    <SourceText group={group} /> · <Link to={links.train(text(served, "id"))}>training</Link>
                  </small>
                </div>
              );
            },
          },
          {
            key: "endpoint",
            label: "Serving",
            sort: (group) => group.published.length,
            render: (group) => {
              const live = group.published[0];
              if (!live) return <Status value="not published" tone="neutral" />;
              return (
                <div className="flex flex-col items-start gap-1">
                  <span className="inline-flex items-center gap-1.5">
                    <Status value="published" tone="success" />
                    <VersionChip n={versionNumber(group, text(live, "id"))} />
                  </span>
                  <CopyMono value={text(live, "id")} short={8} label="Copy the model id" />
                </div>
              );
            },
          },
          {
            key: "calls",
            label: "Calls, 24 h",
            numeric: true,
            sort: (group) => group.versions.reduce((sum, v) => sum + trafficOf(traffic.data, text(v, "id")).calls, 0),
            render: (group) => {
              const calls = group.versions.reduce((sum, v) => sum + trafficOf(traffic.data, text(v, "id")).calls, 0);
              return traffic.data ? <span className="font-mono tabular-nums">{calls}</span> : <span className="muted">—</span>;
            },
          },
          {
            key: "errors",
            label: "Errors",
            numeric: true,
            render: (group) => {
              const live = servedVersion(group);
              const t = trafficOf(traffic.data, text(live, "id"));
              return traffic.data && t.calls ? <span className={t.errors ? "font-mono text-bad" : "font-mono"}>{errorRate(t)}</span> : <span className="muted">—</span>;
            },
          },
          {
            key: "latency",
            label: "Mean latency",
            numeric: true,
            render: (group) => {
              const t = trafficOf(traffic.data, text(servedVersion(group), "id"));
              return t.meanMs === null ? <span className="muted">—</span> : <span className="font-mono tabular-nums">{t.meanMs.toFixed(0)} ms</span>;
            },
          },
          {
            key: "action",
            label: <span className="sr-only">Actions</span>,
            render: (group) => {
              const live = group.published[0];
              const id = text(live ?? group.latest, "id");
              const n = versionNumber(group, id);
              return (
                <div className="flex flex-wrap justify-end gap-1.5">
                  {live && (
                    <Button variant="ghost" onClick={() => setCalling(id)} aria-label={`Call ${group.name}`}>
                      <Code size={14} aria-hidden /> Call
                    </Button>
                  )}
                  {live ? (
                    <ConfirmButton
                      title={`Unpublish ${group.name}?`}
                      confirm={`Unpublishing v${n} stops every outside call at once. You can publish it again later.`}
                      confirmLabel="Unpublish"
                      disabled={busy === id}
                      onConfirm={() => publish(id, true)}
                    >
                      Unpublish
                    </ConfirmButton>
                  ) : (
                    <Button variant="primary" loading={busy === id} onClick={() => void publish(id, false)} title={`Publish the latest version, v${n}`}>
                      Publish v{n}
                    </Button>
                  )}
                </div>
              );
            },
          },
        ]}
      />
      {calling !== null && <CallAModel models={calling ? allPublished.filter((m) => text(m, "id") === calling) : allPublished} onClose={() => setCalling(null)} />}
    </>
  );
}

function CallAModel({ models, onClose }: { models: Row[]; onClose: () => void }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = useOrigin();
  return (
    <Drawer
      title="Call a Model from Your Code"
      subtitle={models.length === 1 ? text(models[0], "name") : "Every published model, ready to paste."}
      onClose={onClose}
      width={640}
      footer={<Button onClick={onClose}>Close</Button>}
    >
      <div id="call-a-model" className="space-y-3">
        {models.length === 0 ? (
          <Empty compact icon={Globe} title="Nothing is published">
            Publish a model to call it from outside Lake.
          </Empty>
        ) : (
          models.map((model) => (
            <CopyBlock
              key={text(model, "id")}
              label={
                <>
                  {text(model, "name")}{" "}
                  <small className="muted">
                    <code>{text(model, "id").slice(0, 8)}</code>
                  </small>
                </>
              }
              value={predictCurl(origin, customerId, workspaceId, model)}
            />
          ))
        )}
        <p className="m-0 text-[13px] text-ink-2">
          Each row you send is keyed by the model's features; the answer ranks the rows by risk with what drives each score. Use a key scoped to <code>predict</code> (each model's page makes one); from an AI agent it is the MCP tool <code>predict</code>.
        </p>
        <p className="m-0 text-[13px] text-ink-2">Unpublishing a model stops every outside call at once.</p>
      </div>
    </Drawer>
  );
}
