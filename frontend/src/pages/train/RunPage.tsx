/** Train → Run (web_ui.cpp workspace_content "run", run_report_panel, run_details_panel): one run,
 * its report once it finished, its lineage, outputs, log and trace. While it runs the page checks
 * again every 15 seconds; the report is asked for only once the run is no longer running, queued
 * or pending (asking a running run makes Zetta drop the connection). */
import { Link, useSearchParams } from "react-router-dom";
import type { Row } from "@/lib/api";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Badge, ErrorBlock, Query } from "@/lib/ui";
import { WorkspaceFrame, cellText, qs, text, useWorkspace, useWorkspaceState, useWq, useWsPath } from "./workspace";
import { ObjectTable, RunSubject, TrialSubject, useStudies, when } from "./RunsPage";

const UNFINISHED = new Set(["running", "queued", "pending"]);

export default function RunPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  const ready = !!useWorkspaceState().data?.ready;
  const run = useWq<Row>("get_run", { run_id: id }, {
    enabled: ready,
    refetchInterval: (query) => (UNFINISHED.has(text(query.state.data as Row | undefined, "status")) ? 15_000 : false),
  });
  useTrail([
    { label: "Train", to: path(`/train/agents?${space}`) },
    { label: "Runs", to: path(`/train/runs?${space}`) },
    entityCrumb(id),
  ]);
  // "Run: <uuid>" told a person nothing (lake#255): the pipeline's name when the run carries it.
  const pipeline = text(run.data, "pipeline_name");
  const title = pipeline ? `Run of ${pipeline}` : id ? `Run: ${id}` : "Run";
  return (
    <WorkspaceFrame eyebrow="Train" title={title}>
      {() => (
        <Query q={run} what="the run">
          {(value) => <RunBody run={value} id={id} />}
        </Query>
      )}
    </WorkspaceFrame>
  );
}

function RunBody({ run, id }: { run: Row; id: string }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const studies = useStudies();
  const status = text(run, "status");
  const finished = !UNFINISHED.has(status);
  const report = useWq<Row>("get_run_report", { run_id: id }, { enabled: finished });
  const traces = useWq<Row>("list_traces", { limit: 500 });
  const trial = studies.byRun.get(text(run, "id"));
  const started = text(run, "created") ? ` · started ${when(text(run, "created"))}` : "";

  const said: string[] = [];
  for (const log of (run.logs ?? []) as Row[]) {
    const message = text(log, "message");
    if (message.startsWith("structural:")) said.push(`structural:${message.slice(12)}`);
    else if (message.includes(" was not read: ")) said.push(message.slice(message.indexOf(" ") + 1));
  }

  return (
    <>
      <p>
        <Link to={path(`/train/experiment/run?${qs({ customer_id: customerId, run: text(run, "id") })}`)}>Training charts</Link>
      </p>
      <p>
        <Link to={path(`/train/streams?${qs({ customer_id: customerId })}`)}>Workspace streams</Link>
      </p>
      <p>
        Status: <Badge>{status || "unknown"}</Badge>
      </p>
      {trial ? (
        <p className="sub">
          <TrialSubject refOf={trial} />
          {started}
        </p>
      ) : text(run, "graph") ? (
        <p className="sub">
          <RunSubject run={run} />
          {started}
        </p>
      ) : null}
      {said.length > 0 && (
        <section className="panel notice" role="status">
          {said.map((line, i) =>
            line.startsWith("structural:") ? (
              <p key={i}>
                <strong>Nothing was trained.</strong> {line.slice("structural:".length)}.
              </p>
            ) : (
              <p key={i}>{line}.</p>
            ),
          )}
        </section>
      )}
      {report.isError ? <ErrorBlock error={report.error} what="the report" retry={() => void report.refetch()} /> : null}
      {report.data ? <ReportPanel report={report.data} /> : null}
      <Details run={run} spans={(traces.data?.spans ?? []) as Row[]} id={id} />
      {traces.isError ? <ErrorBlock error={traces.error} what="the trace" retry={() => void traces.refetch()} /> : null}
      {!finished && !report.data && (
        <div className="panel" role="status">
          The report appears when the run finishes. This page checks again every 15 seconds.
        </div>
      )}
    </>
  );
}

/* ------------------------------------------------------------------ the report */

const HELD_OUT: [string, string][] = [
  ["held_out_auc", "AUC"],
  ["held_out_log_loss", "log loss"],
  ["held_out_accuracy", "accuracy"],
  ["held_out_rmse", "RMSE"],
  ["held_out_mae", "MAE"],
];

/** The score a reader looks for first (lake#449): the held-out figure and how many rows it was
 * measured on, any caveat on it beside it, then the model's other figures. */
function Score({ report }: { report: Row }) {
  const model = report.model;
  if (!model || typeof model !== "object" || Array.isArray(model)) return null;
  const number = (key: string): number | null => (typeof (model as Row)[key] === "number" ? ((model as Row)[key] as number) : null);
  const caveat = (field: string) =>
    ((report.caveats ?? []) as Row[])
      .filter((item) => item && typeof item === "object" && text(item, "field") === field)
      .map((item) => `${text(item, "why")}.`)
      .join(" ");
  const rows = number("held_out_rows");
  const size = rows !== null ? ` on ${rows.toFixed(0)} rows the model did not see` : " on rows the model did not see";
  const figures = HELD_OUT.filter(([key]) => number(key) !== null);
  const numeric = Object.entries(model as Row).filter(([key, value]) => typeof value === "number" && key !== "held_out_rows");
  return (
    <>
      <h3>Score</h3>
      {figures.length === 0 ? (
        number("in_sample_auc") !== null ? (
          <p>
            No held-out score. In-sample AUC {number("in_sample_auc")!.toFixed(3)} was measured on the rows the model learned from and flatters it; it is
            not a result.
          </p>
        ) : (
          <p>No held-out score was measured for this run.</p>
        )
      ) : (
        figures.map(([key, label], index) => {
          const why = caveat(key);
          const value = number(key)!.toFixed(3);
          return index === 0 ? (
            <div key={key}>
              <p>
                Held-out {label} <strong>{value}</strong>
                {size}
              </p>
              {why && (
                <p className="notice status-warning" role="status">
                  <Badge tone="warning">do not quote</Badge> {why}
                </p>
              )}
            </div>
          ) : (
            <p key={key} className="sub">
              Held-out {label} {value}
              {why ? ` — ${why}` : ""}
            </p>
          );
        })
      )}
      {(numeric.length > 0 || rows !== null) && (
        <details>
          <summary>Every figure the model records</summary>
          <dl>
            {numeric.map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key}</dt>
                <dd>{cellText(value)}</dd>
              </div>
            ))}
            {rows !== null && (
              <div className="contents">
                <dt>held_out_rows</dt>
                <dd>{rows.toFixed(0)}</dd>
              </div>
            )}
          </dl>
        </details>
      )}
    </>
  );
}

function ReportPanel({ report }: { report: Row }) {
  const dataset = report.dataset;
  const section = (key: string, heading: string, note: string) => {
    const rows = report[key];
    if (!Array.isArray(rows) || rows.length === 0) return null;
    return (
      <>
        <h3>{heading}</h3>
        {note && <p className="muted">{note}</p>}
        <ObjectTable rows={rows} />
      </>
    );
  };
  return (
    <section className="panel">
      <h2>Report</h2>
      <Score report={report} />
      {dataset && typeof dataset === "object" && !Array.isArray(dataset) && (
        <>
          <h3>What It Learned From</h3>
          <dl>
            {Object.entries(dataset as Row).map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key === "source" ? "trained on" : key === "not_used" ? "columns not used" : key}</dt>
                <dd>{Array.isArray(value) ? value.map(cellText).join(", ") : cellText(value)}</dd>
              </div>
            ))}
          </dl>
        </>
      )}
      {section("drivers", "What the Model Leans On", "A weight is what the model relied on, not a cause.")}
      {section("at_risk", "Most at Risk", "")}
      {section("caveats", "Do Not Read These as Results", "")}
      {section("missing", "What This Run Cannot Tell You", "")}
      <details>
        <summary>The raw report</summary>
        <pre>{JSON.stringify(report, null, 2)}</pre>
      </details>
    </section>
  );
}

/* ------------------------------------------------------------------ lineage, outputs, log, trace */

function Details({ run, spans, id }: { run: Row; spans: Row[]; id: string }) {
  const lineage = run.lineage;
  const outputs = run.outputs;
  const logs = run.logs;
  // The trace: spans whose attributes name this run, the longest bar scaled to the widest.
  const mine = spans.filter((span) => Object.values((span.attributes ?? {}) as Row).some((value) => typeof value === "string" && value === id));
  const longest = Math.max(0, ...mine.map((span) => (typeof span.duration_ms === "number" ? span.duration_ms : 0)));
  const filled = (value: unknown) => (Array.isArray(value) ? value.length > 0 : value && typeof value === "object" ? Object.keys(value).length > 0 : !!value);
  return (
    <>
      {lineage && typeof lineage === "object" && !Array.isArray(lineage) && (
        <section className="panel">
          <h2>Lineage</h2>
          <dl>
            {Object.entries(lineage as Row).map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key}</dt>
                <dd>{Array.isArray(value) ? value.map(cellText).join(" → ") : cellText(value)}</dd>
              </div>
            ))}
          </dl>
        </section>
      )}
      {filled(outputs) && (
        <section className="panel">
          <h2>Outputs</h2>
          <ObjectTable rows={outputs} />
        </section>
      )}
      {filled(logs) && (
        <section className="panel">
          <h2>Log</h2>
          <ObjectTable rows={logs} />
        </section>
      )}
      {mine.length > 0 && (
        <section className="panel">
          <h2>Trace</h2>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Span</th>
                  <th>Time</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {mine.map((span, i) => {
                  const duration = typeof span.duration_ms === "number" ? span.duration_ms : 0;
                  return (
                    <tr key={i}>
                      <td>{text(span, "name")}</td>
                      <td>{duration.toFixed(1)} ms</td>
                      <td>
                        <span
                          className={span.ok === false ? "waterfall-bar failed" : "waterfall-bar"}
                          style={{ width: longest > 0 ? Math.trunc(4 + (240 * duration) / longest) : 4 }}
                        />
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        </section>
      )}
    </>
  );
}
