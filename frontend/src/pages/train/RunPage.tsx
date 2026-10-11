/** Train → Run (web_ui.cpp workspace_content "run", run_report_panel, run_details_panel): one run,
 * its report once it finished, its lineage, outputs, log and trace, beside its facts. While it runs
 * the page checks again every 15 seconds; the report is asked for only once the run is no longer
 * running, queued or pending (asking a running run makes Zetta drop the connection). */
import { useSearchParams } from "react-router-dom";
import { Activity, LineChart } from "lucide-react";
import type { Row } from "@/lib/api";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, ErrorBlock, KeyValue, Mono, Query, Split, Stat, Status } from "@/lib/ui";
import { WorkspaceFrame, cellText, qs, text, timeAgo, useWorkspace, useWorkspaceState, useWq, useWsPath } from "./workspace";
import { ObjectTable, RunSubject, TrialSubject, UNFINISHED, runStatusTone, useExperimentRefs, when } from "./RunsPage";
import { evidenceOf } from "./evidence";

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
  const status = text(run.data, "status");
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title={
        run.data ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {title}
            <Status value={status || "unknown"} tone={runStatusTone(status)} />
          </span>
        ) : (
          title
        )
      }
      actions={
        id ? (
          <>
            <Button to={path(`/train/streams?${qs({ customer_id: customerId })}`)}>
              <Activity size={14} aria-hidden /> Workspace streams
            </Button>
            <Button to={path(`/train/experiment/run?${qs({ customer_id: customerId, run: text(run.data, "id") || id })}`)}>
              <LineChart size={14} aria-hidden /> Training charts
            </Button>
          </>
        ) : null
      }
    >
      {() => (
        <Query q={run} what="the run">
          {(value) => <RunBody run={value} id={id} />}
        </Query>
      )}
    </WorkspaceFrame>
  );
}

function RunBody({ run, id }: { run: Row; id: string }) {
  const experiments = useExperimentRefs();
  const status = text(run, "status");
  const finished = !UNFINISHED.has(status);
  const report = useWq<Row>("get_run_report", { run_id: id }, { enabled: finished });
  const traces = useWq<Row>("list_traces", { limit: 500 });
  const trial = experiments.byRun.get(text(run, "id"));
  const created = text(run, "created");

  const said: string[] = [];
  for (const log of (run.logs ?? []) as Row[]) {
    const message = text(log, "message");
    if (message.startsWith("structural:")) said.push(`structural:${message.slice(12)}`);
    else if (message.includes(" was not read: ")) said.push(message.slice(message.indexOf(" ") + 1));
  }

  return (
    <Split
      side={
        <Card label="Facts">
          <KeyValue
            rows={[
              ["Status", <Status key="status" value={status || "unknown"} tone={runStatusTone(status)} />],
              ["Pipeline", trial ? <TrialSubject key="trial" refOf={trial} /> : text(run, "graph") || text(run, "pipeline_name") ? <RunSubject key="subject" run={run} /> : null],
              ["Started", created ? <span title={when(created)}>{`${when(created)} · ${timeAgo(created)}`}</span> : null],
              ["Run", <Mono key="id" short={12}>{text(run, "id") || id}</Mono>],
            ]}
          />
        </Card>
      }
    >
      {said.map((line, i) =>
        line.startsWith("structural:") ? (
          <Banner key={i} tone="warn" title="Nothing was trained.">
            {line.slice("structural:".length)}.
          </Banner>
        ) : (
          <Banner key={i} tone="quiet">
            {line}.
          </Banner>
        ),
      )}
      {!finished && !report.data && <Banner tone="info">The report appears when the run finishes. This page checks again every 15 seconds.</Banner>}
      {report.isError ? <ErrorBlock error={report.error} what="the report" retry={() => void report.refetch()} /> : null}
      {report.data ? <ReportPanel report={report.data} /> : null}
      <Details run={run} spans={(traces.data?.spans ?? []) as Row[]} id={id} />
      {traces.isError ? <ErrorBlock error={traces.error} what="the trace" retry={() => void traces.refetch()} /> : null}
    </Split>
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
  const [first, ...others] = figures;
  return (
    <div className="space-y-3">
      <h3 className="m-0">Score</h3>
      {!first ? (
        number("in_sample_auc") !== null ? (
          <Banner tone="quiet">
            No held-out score. In-sample AUC {number("in_sample_auc")!.toFixed(3)} was measured on the rows the model learned from and flatters it; it is
            not a result.
          </Banner>
        ) : (
          <Banner tone="quiet">No held-out score was measured for this run.</Banner>
        )
      ) : (
        <>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-[minmax(0,16rem)_minmax(0,1fr)]">
            <Stat label={`Held-out ${first[1]}`} value={number(first[0])!.toFixed(3)} tone={caveat(first[0]) ? "bad" : undefined} sub={size.trim()} />
            {others.length > 0 && (
              <div className="flex flex-wrap content-start gap-1.5">
                {others.map(([key, label]) => {
                  const why = caveat(key);
                  return (
                    <Chip key={key} tone={why ? "warn" : "neutral"} title={why || undefined}>
                      Held-out {label} <span className="font-mono">{number(key)!.toFixed(3)}</span>
                    </Chip>
                  );
                })}
              </div>
            )}
          </div>
          {caveat(first[0]) && (
            <Banner tone="warn" title="Do not quote">
              {caveat(first[0])}
            </Banner>
          )}
          {others
            .filter(([key]) => caveat(key))
            .map(([key, label]) => (
              <p key={key} className="muted m-0 text-[12.5px]">
                Held-out {label} {number(key)!.toFixed(3)} — {caveat(key)}
              </p>
            ))}
        </>
      )}
      {(numeric.length > 0 || rows !== null) && (
        <details>
          <summary>Every figure the model records</summary>
          <dl className="mt-2">
            {numeric.map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key}</dt>
                <dd className="font-mono">{cellText(value)}</dd>
              </div>
            ))}
            {rows !== null && (
              <div className="contents">
                <dt>held_out_rows</dt>
                <dd className="font-mono">{rows.toFixed(0)}</dd>
              </div>
            )}
          </dl>
        </details>
      )}
    </div>
  );
}

function ReportPanel({ report }: { report: Row }) {
  const dataset = report.dataset;
  const section = (key: string, heading: string, note: string) => {
    const rows = report[key];
    if (!Array.isArray(rows) || rows.length === 0) return null;
    return (
      <div className="mt-5 space-y-2">
        <h3 className="m-0">{heading}</h3>
        {note && <p className="muted m-0 text-[12.5px]">{note}</p>}
        <ObjectTable rows={rows} />
      </div>
    );
  };
  return (
    <Card label="Report" title="What the Run Found">
      <Score report={report} />
      {dataset && typeof dataset === "object" && !Array.isArray(dataset) && (
        <div className="mt-5 space-y-2">
          <h3 className="m-0">What It Learned From</h3>
          <dl>
            {Object.entries(dataset as Row).map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key === "source" ? "trained on" : key === "not_used" ? "columns not used" : key}</dt>
                <dd>{Array.isArray(value) ? value.map(cellText).join(", ") : cellText(value)}</dd>
              </div>
            ))}
          </dl>
        </div>
      )}
      {section("drivers", "What the Model Leans On", "A weight is what the model relied on, not a cause.")}
      {Array.isArray(report.at_risk) && report.at_risk.length > 0 && (
        <div className="mt-5 space-y-2">
          <h3 className="m-0">Most at Risk</h3>
          <AtRiskTable rows={report.at_risk as Row[]} />
        </div>
      )}
      {section("caveats", "Do Not Read These as Results", "")}
      {section("missing", "What This Run Cannot Tell You", "")}
      <details className="mt-5">
        <summary>The raw report</summary>
        <pre>{JSON.stringify(report, null, 2)}</pre>
      </details>
    </Card>
  );
}

/** The ranked at-risk rows with their evidence in words (lake#581): the row's values as
 * "feature = value" chips and its top reasons with their effect, never the raw JSON. */
function AtRiskTable({ rows }: { rows: Row[] }) {
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th scope="col">Rank</th>
            <th scope="col">Entity</th>
            <th scope="col" className="text-right">
              Risk
            </th>
            <th scope="col">Evidence</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((row, i) => {
            const { values, reasons } = evidenceOf(row.evidence);
            return (
              <tr key={i}>
                <td className="font-mono">{cellText(row.rank)}</td>
                <td className="font-mono">{cellText(row.entity)}</td>
                <td className="text-right font-mono">{typeof row.risk === "number" ? row.risk.toFixed(3) : cellText(row.risk)}</td>
                <td>
                  {values.length > 0 && (
                    <div className="flex flex-wrap gap-1">
                      {values.map((v) => (
                        <Chip key={v.name}>
                          {v.name} = <span className="font-mono">{v.value}</span>
                        </Chip>
                      ))}
                    </div>
                  )}
                  {reasons.length > 0 && (
                    <ol className="mt-1 mb-0 pl-4 text-[12.5px] text-ink-2">
                      {reasons.map((r) => (
                        <li key={r.reason}>
                          <span className="font-medium text-ink">{r.reason}</span>
                          {r.label && <> {r.label}</>}
                        </li>
                      ))}
                    </ol>
                  )}
                  {values.length === 0 && reasons.length === 0 && <span className="text-ink-3">—</span>}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
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
        <Card label="Lineage">
          <dl>
            {Object.entries(lineage as Row).map(([key, value]) => (
              <div key={key} className="contents">
                <dt>{key}</dt>
                <dd>{Array.isArray(value) ? value.map(cellText).join(" → ") : cellText(value)}</dd>
              </div>
            ))}
          </dl>
        </Card>
      )}
      {filled(outputs) && (
        <Card label="Outputs">
          <ObjectTable rows={outputs} />
        </Card>
      )}
      {filled(logs) && (
        <Card label="Log">
          <ObjectTable rows={logs} />
        </Card>
      )}
      {mine.length > 0 && (
        <Card label="Trace">
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Span</th>
                  <th className="text-right">Time</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {mine.map((span, i) => {
                  const duration = typeof span.duration_ms === "number" ? span.duration_ms : 0;
                  return (
                    <tr key={i}>
                      <td>{text(span, "name")}</td>
                      <td className="text-right font-mono tabular-nums">{duration.toFixed(1)} ms</td>
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
        </Card>
      )}
    </>
  );
}
