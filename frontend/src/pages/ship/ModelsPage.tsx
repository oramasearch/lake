/** Ship → Models (web_ui.cpp workspace_content "models"): trained models, whether each is published
 * as a prediction endpoint, and how to call a published one. Publish and Unpublish call
 * `expose_model` / `unexpose_model` in the workspace, as `/ship/models/publish` did. */
import { useState } from "react";
import { Link } from "react-router-dom";
import type { Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Badge, Button, CopyBlock, DataTable, ErrorBlock, Query } from "@/lib/ui";
import { WorkspaceFrame, qs, text, useWorkspace, useWq, useWsPath, wa } from "../train/workspace";
import { TrialSubject, useStudies, when, type StudyRef } from "../train/RunsPage";

export default function ModelsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Ship", to: path(`/ship/models?${space}`) },
    { label: "Models", to: path(`/ship/models?${space}`) },
  ]);
  return (
    <WorkspaceFrame eyebrow="Ship" title="Models" sub="Trained models, and whether each is published as a prediction endpoint.">
      {() => <Models />}
    </WorkspaceFrame>
  );
}

/** A held-out score when the run measured one; otherwise say it is in-sample, or not measured. */
function Score({ metrics }: { metrics: Row }) {
  for (const key of ["held_out_auc", "holdout_auc"]) {
    if (typeof metrics[key] === "number") {
      return (
        <>
          AUC {(metrics[key] as number).toFixed(3)} <small>held out</small>
        </>
      );
    }
  }
  if (typeof metrics.in_sample_auc === "number") {
    return (
      <>
        AUC {metrics.in_sample_auc.toFixed(3)} <small className="muted">in-sample only, not held out</small>
      </>
    );
  }
  return <span className="muted">not measured</span>;
}

function Models() {
  const path = useWsPath();
  const scope = useWorkspace();
  const q = useWq<Row>("list_models");
  const studies = useStudies();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState<string | null>(null);

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

  return (
    <>
      {error ? <ErrorBlock error={error} /> : null}
      <Query
        q={q}
        what="models"
        isEmpty={(v) => !((v.models ?? []) as Row[]).length}
        empty={<div className="panel empty">No models yet. Models appear here when a run trains one.</div>}
      >
        {(value) => {
          const models = (value.models ?? []) as Row[];
          const published = models.filter((model) => model.exposed === true);
          return (
            <>
              <DataTable<Row>
                rows={models}
                what="models"
                rowKey={(model, i) => text(model, "id") || String(i)}
                columns={[
                  {
                    key: "model",
                    label: "Model",
                    render: (model) => (
                      <>
                        <strong>{text(model, "name")}</strong>
                        {studies.promoted.has(text(model, "id")) && (
                          <>
                            {" "}
                            <Badge tone="success">promoted</Badge>
                          </>
                        )}
                        <br />
                        <small className="muted">
                          <code>{text(model, "id").slice(0, 8)}</code>
                        </small>
                      </>
                    ),
                  },
                  {
                    key: "project",
                    label: "Project",
                    render: (model) => {
                      // A study's model names its study (lake#449); a dataset-level study has no project.
                      const project = text(model, "project_name");
                      const study = studyOf(model);
                      const link = project ? <Link to={path(`/design/project?${qs({ customer_id: scope.customerId, name: project })}`)}>{project}</Link> : null;
                      if (!link && !study)
                        return <span className="muted">{text(model, "project") ? "a deleted project" : "no project"}</span>;
                      if (!link) return <TrialSubject refOf={study!} />;
                      return (
                        <>
                          {link}
                          {study && (
                            <>
                              <br />
                              <small>
                                <TrialSubject refOf={study} />
                              </small>
                            </>
                          )}
                        </>
                      );
                    },
                  },
                  { key: "created", label: "Trained", render: (model) => when(text(model, "created")) },
                  { key: "score", label: "Score", render: (model) => <Score metrics={(model.metrics ?? {}) as Row} /> },
                  {
                    key: "published",
                    label: "Published",
                    render: (model) => (model.exposed === true ? <Badge tone="success">published</Badge> : <Badge>not published</Badge>),
                  },
                  {
                    key: "action",
                    label: "",
                    render: (model) => {
                      const id = text(model, "id");
                      const on = model.exposed === true;
                      return (
                        <Button variant={on ? "secondary" : "primary"} loading={busy === id} onClick={() => void publish(id, on)}>
                          {on ? "Unpublish" : "Publish"}
                        </Button>
                      );
                    },
                  },
                ]}
              />
              <CallAModel models={published} />
            </>
          );
        }}
      </Query>
    </>
  );
}

function CallAModel({ models }: { models: Row[] }) {
  const { customerId, workspaceId } = useWorkspace();
  const origin = window.location.origin;
  return (
    <section className="panel" id="call-a-model">
      <h2>Call a Model from Your Code</h2>
      {models.length === 0 ? (
        <p>Publish a model to call it from outside lake.</p>
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
      <p>
        Each row you send is keyed by the model's features; the answer ranks the rows by risk with what drives each score. Use a lake key from{" "}
        <Link to="/developer/mcp">Developer → MCP</Link>; from an AI agent it is the MCP tool <code>predict</code>. Unpublishing a model stops every
        outside call at once.
      </p>
    </section>
  );
}
