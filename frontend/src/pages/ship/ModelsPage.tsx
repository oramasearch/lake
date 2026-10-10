/** Ship → Models (web_ui.cpp workspace_content "models"): trained models, whether each is published
 * as a prediction endpoint, and how to call a published one. Publish and Unpublish call
 * `expose_model` / `unexpose_model` in the workspace, as `/ship/models/publish` did. */
import { useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { Brain, Code, Globe, Sparkles, Trophy } from "lucide-react";
import type { Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Drawer, ConfirmButton } from "@/lib/overlay";
import { useTrail } from "@/lib/trail";
import { Button, Chip, CopyBlock, DataTable, Empty, ErrorBlock, FilterChips, Query, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, text, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";
import { TrialSubject, useStudies, when, type StudyRef } from "../train/RunsPage";
import { CopyMono } from "./parts";

export default function ModelsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Ship", to: path(`/ship/models?${space}`) },
    { label: "Models", to: path(`/ship/models?${space}`) },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Ship"
      title="Models"
      sub="Trained models, and whether each is published as a prediction endpoint."
      actions={
        <Button to={path(`/train/runs?${space}`)}>
          <Sparkles size={14} aria-hidden /> Runs
        </Button>
      }
    >
      {() => <Models />}
    </WorkspaceFrame>
  );
}

function heldOut(metrics: Row): number | null {
  for (const key of ["held_out_auc", "holdout_auc"]) if (typeof metrics[key] === "number") return metrics[key] as number;
  return null;
}

/** A held-out score when the run measured one; otherwise say it is in-sample, or not measured. */
function Score({ metrics }: { metrics: Row }) {
  const held = heldOut(metrics);
  if (held !== null) {
    return (
      <span className="whitespace-nowrap">
        <span className="font-mono tabular-nums">AUC {held.toFixed(3)}</span> <small className="muted">held out</small>
      </span>
    );
  }
  if (typeof metrics.in_sample_auc === "number") {
    return (
      <span>
        <span className="font-mono tabular-nums">AUC {metrics.in_sample_auc.toFixed(3)}</span>
        <br />
        <small className="text-warn">in-sample only, not held out</small>
      </span>
    );
  }
  return <span className="muted">not measured</span>;
}

type Filter = "published" | "unpublished";

function Models() {
  const path = useWsPath();
  const scope = useWorkspace();
  const q = useWq<Row>("list_models");
  const studies = useStudies();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [needle, setNeedle] = useState("");
  const [state, setState] = useState<Filter | null>(null);
  const [kind, setKind] = useState<string | null>(null);
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

  const studyOf = (model: Row): StudyRef | undefined => studies.byModel.get(text(model, "id")) ?? studies.byRun.get(text(model, "run"));
  const models = useMemo(() => ((q.data?.models ?? []) as Row[]), [q.data]);
  const kinds = useMemo(() => {
    const counts = new Map<string, number>();
    for (const model of models) {
      const k = text(model, "kind");
      if (k) counts.set(k, (counts.get(k) ?? 0) + 1);
    }
    return [...counts.entries()].map(([id, count]) => ({ id, label: id.replace(/[_-]+/g, " "), count }));
  }, [models]);

  return (
    <>
      {error ? <ErrorBlock error={error} /> : null}
      <Query
        q={q}
        what="models"
        lines={6}
        isEmpty={(v) => !((v.models ?? []) as Row[]).length}
        empty={
          <Empty icon={Brain} title="No models yet" action={<Button to={path(`/train/runs?${qs({ customer_id: scope.customerId })}`)}>Go to Runs</Button>}>
            Models appear here when a run trains one.
          </Empty>
        }
      >
        {(value) => {
          const all = (value.models ?? []) as Row[];
          const published = all.filter((model) => model.exposed === true);
          const scored = all.filter((model) => heldOut((model.metrics ?? {}) as Row) !== null).length;
          const promoted = all.filter((model) => studies.promoted.has(text(model, "id"))).length;
          const lower = needle.trim().toLowerCase();
          const rows = all.filter(
            (model) =>
              (!lower || ["name", "id", "kind", "project_name", "pipeline_name"].some((k) => text(model, k).toLowerCase().includes(lower))) &&
              (!state || (state === "published") === (model.exposed === true)) &&
              (!kind || text(model, "kind") === kind),
          );
          return (
            <>
              <StatRow>
                <Stat label="Models" value={all.length} icon={Brain} sub={`${all.filter((m) => m.has_weights === true).length} with weights`} />
                <Stat label="Published" value={published.length} icon={Globe} tone={published.length ? "ok" : undefined} sub="answering outside calls" />
                <Stat label="Held-out score" value={scored} sub="measured on held-out rows" />
                <Stat label="Promoted" value={promoted} icon={Trophy} sub="by a study" />
              </StatRow>
              <Toolbar>
                <SearchInput value={needle} onChange={setNeedle} placeholder="Search models…" label="Search models" />
                <FilterChips<Filter>
                  options={[
                    { id: "published", label: "Published", count: published.length },
                    { id: "unpublished", label: "Not published", count: all.length - published.length },
                  ]}
                  value={state}
                  onChange={setState}
                />
                {kinds.length > 1 && <FilterChips options={kinds} value={kind} onChange={setKind} allLabel="Every kind" />}
                <div className="ml-auto">
                  <Button onClick={() => setCalling("")}>
                    <Code size={14} aria-hidden /> Call a model
                  </Button>
                </div>
              </Toolbar>
              <DataTable<Row>
                rows={rows}
                what="models"
                caption="Models"
                rowKey={(model, i) => text(model, "id") || String(i)}
                empty={<Empty compact title="No model matches">Clear the search or a filter to see more.</Empty>}
                initialSort={{ key: "created", dir: "descending" }}
                columns={[
                  {
                    key: "model",
                    label: "Model",
                    sort: (model) => text(model, "name").toLowerCase(),
                    render: (model) => (
                      <div className="min-w-0">
                        <div className="flex flex-wrap items-center gap-1.5">
                          <strong className="text-ink">{text(model, "name")}</strong>
                          {text(model, "kind") && <Chip>{text(model, "kind").replace(/[_-]+/g, " ")}</Chip>}
                          {studies.promoted.has(text(model, "id")) && <Chip tone="ok">promoted</Chip>}
                        </div>
                        <small className="muted">
                          <code>{text(model, "id").slice(0, 8)}</code>
                          {text(model, "pipeline_name") && <> · {text(model, "pipeline_name")}</>}
                        </small>
                      </div>
                    ),
                  },
                  {
                    key: "project",
                    label: "Project",
                    sort: (model) => text(model, "project_name").toLowerCase(),
                    render: (model) => {
                      // A study's model names its study (lake#449); a dataset-level study has no project.
                      const project = text(model, "project_name");
                      const study = studyOf(model);
                      const link = project ? <Link to={path(`/design/project?${qs({ customer_id: scope.customerId, name: project })}`)}>{project}</Link> : null;
                      if (!link && !study) return <span className="muted">{text(model, "project") ? "a deleted project" : "no project"}</span>;
                      if (!link) return <TrialSubject refOf={study!} />;
                      return (
                        <>
                          {link}
                          {study && (
                            <>
                              <br />
                              <small className="muted">
                                <TrialSubject refOf={study} />
                              </small>
                            </>
                          )}
                        </>
                      );
                    },
                  },
                  {
                    key: "created",
                    label: "Trained",
                    sort: (model) => text(model, "created"),
                    render: (model) => <span title={when(text(model, "created"))}>{relative(text(model, "created"), Date.now(), "—")}</span>,
                  },
                  {
                    key: "score",
                    label: "Score",
                    sort: (model) => heldOut((model.metrics ?? {}) as Row) ?? -1,
                    render: (model) => <Score metrics={(model.metrics ?? {}) as Row} />,
                  },
                  {
                    key: "published",
                    label: "Endpoint",
                    sort: (model) => (model.exposed === true ? 1 : 0),
                    render: (model) =>
                      model.exposed === true ? (
                        <div className="flex flex-col items-start gap-1">
                          <Status value="published" tone="success" />
                          <CopyMono value={text(model, "id")} short={8} label="Copy the model id" />
                        </div>
                      ) : (
                        <Status value="not published" tone="neutral" />
                      ),
                  },
                  {
                    key: "action",
                    label: <span className="sr-only">Actions</span>,
                    render: (model) => {
                      const id = text(model, "id");
                      const on = model.exposed === true;
                      return (
                        <div className="flex flex-wrap justify-end gap-1.5">
                          {on && (
                            <Button variant="ghost" onClick={() => setCalling(id)} aria-label={`Call ${text(model, "name")}`}>
                              <Code size={14} aria-hidden /> Call
                            </Button>
                          )}
                          {on ? (
                            <ConfirmButton
                              title={`Unpublish ${text(model, "name")}?`}
                              confirm="Unpublishing a model stops every outside call at once. You can publish it again later."
                              confirmLabel="Unpublish"
                              disabled={busy === id}
                              onConfirm={() => publish(id, true)}
                            >
                              Unpublish
                            </ConfirmButton>
                          ) : (
                            <Button variant="primary" loading={busy === id} onClick={() => void publish(id, false)}>
                              Publish
                            </Button>
                          )}
                        </div>
                      );
                    },
                  },
                ]}
              />
              {calling !== null && (
                <CallAModel models={calling ? published.filter((m) => text(m, "id") === calling) : published} onClose={() => setCalling(null)} />
              )}
            </>
          );
        }}
      </Query>
    </>
  );
}

function CallAModel({ models, onClose }: { models: Row[]; onClose: () => void }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = window.location.origin;
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
            Publish a model to call it from outside lake.
          </Empty>
        ) : (
          models.map((model) => {
            const id = text(model, "id");
            const features = (((model.feature_schema ?? {}) as Row).features ?? []) as unknown[];
            const row = features.map((feature) => `"${typeof feature === "string" ? feature : JSON.stringify(feature)}": 0`).join(", ");
            const command =
              `curl -s ${origin}/v1/predict \\\n  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \\\n` +
              `  -d '{"customer_id": "${customerId}", "workspace_id": "${workspaceId || "<workspace id>"}", "model_id": "${id}", "rows": [{${row}}]}'`;
            return (
              <CopyBlock
                key={id}
                label={
                  <>
                    {text(model, "name")}{" "}
                    <small className="muted">
                      <code>{id.slice(0, 8)}</code>
                    </small>
                  </>
                }
                value={command}
              />
            );
          })
        )}
        <p className="m-0 text-[13px] text-ink-2">
          Each row you send is keyed by the model's features; the answer ranks the rows by risk with what drives each score. Use a lake key from{" "}
          <Link to="/developer/mcp">Developer → MCP</Link>; from an AI agent it is the MCP tool <code>predict</code>.
        </p>
        <p className="m-0 text-[13px] text-ink-2">Unpublishing a model stops every outside call at once.</p>
      </div>
    </Drawer>
  );
}
