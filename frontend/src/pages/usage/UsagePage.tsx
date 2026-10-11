/** Usage (`/usage`, lake#564): every dimension of what the chosen workspaces store and use, over a
 * period against the one before, as figures, a chart stacked by workspace or by kind, tables, limit
 * meters and a CSV export. The workspace multi-select is Home's (`?ws=`); `?range=` picks the period
 * and `?dimension=` the chart. Each account's workspaces are read with `usage_over_time` and its
 * limits with `usage_limits`; several accounts' readings are added up here. The operator opens one
 * account with `?customer_id=`. There is no price here; Billing (pages/billing) prices usage where the lake bills. */
import { useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { useQueries, useQuery } from "@tanstack/react-query";
import { ArrowDown, ArrowUp, Download, Gauge, Layers, Users } from "lucide-react";
import { act, ApiError, read, type Row } from "@/lib/api";
import { useSession, type WorkspaceChoice } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { Modal } from "@/lib/overlay";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, Form, Meter, Page, Skeleton, Split, Stat, StateTabs, cx, fieldError } from "@/lib/ui";
import { useWorkspaceSelection, WorkspaceToggles } from "@/components/WorkspaceToggles";
import { UsageChart } from "./UsageChart";
import {
  RANGES,
  breakdownName,
  change,
  formatValue,
  fromLimit,
  limitNote,
  limitScale,
  limitTone,
  mergeReadings,
  rangeOf,
  stackSeries,
  toCsv,
  toLimit,
  windows,
  type Dimension,
  type RangeKey,
  type Reading,
} from "./usage";

const STORE = "lake-usage-workspaces";

type Limit = { dimension: string; title: string; unit: string; limit: number; warn_percent: number; used: number | null; percent: number | null; state: string; enforced: boolean; period: string };
type Limits = { customer_id: string; month: string; limits: Limit[]; warnings: string[] };

async function readUsage(customerId: string, ids: string[] | null, from: string, to: string, granularity: string, signal: AbortSignal): Promise<Reading> {
  return read<Reading>("usage_over_time", { customer_id: customerId, workspace_ids: ids ?? undefined, from, to, granularity }, { workspace: null, signal });
}

export default function UsagePage() {
  const session = useSession();
  const { viewer } = session;
  const [params, setParams] = useSearchParams();
  const asked = params.get("customer_id") ?? "";
  const range = rangeOf(params.get("range"));
  const [stackBy, setStackBy] = useState<"workspace" | "kind">("workspace");
  const [limitOpen, setLimitOpen] = useState<string | null>(null);
  useTrail([{ label: "Usage" }], "Usage");

  // The operator reads one account by its id; a person reads their own workspaces.
  const accountWorkspaces = useQuery({
    queryKey: ["list_workspaces", "", { customer_id: asked }, "usage"],
    queryFn: async ({ signal }) => {
      const answer = await read<Row>("list_workspaces", { customer_id: asked }, { workspace: null, signal });
      return ((answer.workspaces ?? []) as Row[]).filter((w) => w.desired !== "deleted").map((w) => ({ id: String(w.id), name: String(w.name), customer_id: asked }) as WorkspaceChoice);
    },
    enabled: !!asked,
  });
  const workspaces: WorkspaceChoice[] = asked ? (accountWorkspaces.data ?? []) : session.workspaces;
  const selection = useWorkspaceSelection(workspaces, STORE);
  const { chosen, all } = selection;
  const names = useMemo(() => Object.fromEntries(workspaces.map((w) => [w.id, w.name])), [workspaces]);

  // One reading per account the chosen workspaces belong to, for this period and the one before.
  const spaces = useMemo(() => {
    const by = new Map<string, string[]>();
    for (const w of chosen) by.set(w.customer_id, [...(by.get(w.customer_id) ?? []), w.id]);
    return [...by.entries()].map(([customerId, ids]) => ({ customerId, ids: all ? null : ids }));
  }, [chosen, all]);
  const span = useMemo(() => windows(range), [range]);
  const current = useQueries({
    queries: spaces.map((s) => ({
      queryKey: ["usage_over_time", s.customerId, s.ids, span.current],
      queryFn: ({ signal }: { signal: AbortSignal }) => readUsage(s.customerId, s.ids, span.current.from, span.current.to, span.current.granularity, signal),
      staleTime: 60_000,
    })),
  });
  const previous = useQueries({
    queries: spaces.map((s) => ({
      queryKey: ["usage_over_time", s.customerId, s.ids, span.previous],
      queryFn: ({ signal }: { signal: AbortSignal }) => readUsage(s.customerId, s.ids, span.previous.from, span.previous.to, span.previous.granularity, signal),
      staleTime: 300_000,
    })),
  });
  const limits = useQueries({
    queries: spaces.map((s) => ({
      queryKey: ["usage_limits", s.customerId],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<Limits>("usage_limits", { customer_id: s.customerId }, { workspace: null, signal }),
      staleTime: 60_000,
    })),
  });

  const reading = current.every((q) => q.data) ? mergeReadings(current.map((q) => q.data!)) : null;
  const before = previous.every((q) => q.data) ? mergeReadings(previous.map((q) => q.data!)) : null;
  const failed = current.find((q) => q.isError);
  const dimension = params.get("dimension") ?? "storage";
  const shown = reading?.dimensions.find((d) => d.dimension === dimension) ?? reading?.dimensions[0] ?? null;
  const spaceNames = Object.fromEntries(viewer.spaces.map((s) => [s.id, s.label || s.name]));
  const singleSpace = spaces.length === 1 ? spaces[0].customerId : asked || null;

  const withParam = (key: string, value: string | null) => {
    const next = new URLSearchParams(params);
    if (value) next.set(key, value);
    else next.delete(key);
    const query = next.toString();
    return query ? `?${query}` : "?";
  };
  const setParam = (key: string, value: string | null) => {
    const next = new URLSearchParams(params);
    if (value) next.set(key, value);
    else next.delete(key);
    setParams(next, { replace: true });
  };

  const exportCsv = () => {
    if (!reading) return;
    const url = URL.createObjectURL(new Blob([toCsv(reading, names)], { type: "text/csv" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = `lake-usage-${span.current.from.slice(0, 10)}-${range}.csv`;
    link.click();
    URL.revokeObjectURL(url);
  };

  const rangeInfo = RANGES.find((r) => r.key === range)!;
  return (
    <Page
      eyebrow={viewer.administrator ? "Administration" : "Organization"}
      title="Usage"
      sub="What your workspaces store and use over time, against your limits."
      actions={
        <>
          {viewer.administrator && (
            <Button to="/usage/accounts">
              <Users size={14} aria-hidden /> Every account
            </Button>
          )}
          <Button onClick={exportCsv} disabled={!reading}>
            <Download size={14} aria-hidden /> Export CSV
          </Button>
          {singleSpace && (
            <Button variant="primary" onClick={() => setLimitOpen(singleSpace)}>
              <Gauge size={14} aria-hidden /> Set a limit
            </Button>
          )}
        </>
      }
      wide
    >
      {asked && accountWorkspaces.isError ? (
        <ErrorBlock error={accountWorkspaces.error} retry={() => void accountWorkspaces.refetch()} what="this account's workspaces" />
      ) : workspaces.length === 0 ? (
        accountWorkspaces.isLoading ? (
          <Skeleton lines={4} />
        ) : (
          <Empty icon={Layers} title={asked ? "This account has no workspaces" : "Start your first workspace"} action={asked ? undefined : <Button to="/workspaces" variant="primary">Workspaces</Button>}>
            Usage is kept per workspace, so there is nothing to show until one exists.
          </Empty>
        )
      ) : (
        <>
          <WorkspaceToggles workspaces={workspaces} selection={selection} />
<div className="max-w-full overflow-x-auto">
                      <StateTabs label="Period" value={range} onChange={(value: RangeKey) => setParam("range", value === "month" ? null : value)} tabs={RANGES.map((r) => ({ value: r.key, label: r.label }))} />
          </div>
          {limits.flatMap((q) => q.data?.warnings ?? []).map((warning, i) => (
            <Banner key={i} tone="warn" title="Near a Limit">
              {warning}
            </Banner>
          ))}
          {failed ? (
            <ErrorBlock error={failed.error} retry={() => current.forEach((q) => void q.refetch())} what="usage" />
          ) : !reading ? (
            <Skeleton lines={8} />
          ) : (
            <>
              <Figures reading={reading} before={before} previousLabel={rangeInfo.previous} selected={shown?.dimension ?? ""} hrefFor={(d) => withParam("dimension", d === "storage" ? null : d)} />
              <Split
                side={
                  <>
                    <LimitsCard limits={limits.map((q) => q.data).filter((x): x is Limits => !!x)} spaceNames={spaceNames} onSet={setLimitOpen} />
                    {reading.not_measured.length > 0 && (
                      <Banner tone="quiet" title="Not Measured Yet">
                        <ul className="m-0 pl-4">
                          {reading.not_measured.map((line, i) => (
                            <li key={i}>{line}</li>
                          ))}
                        </ul>
                      </Banner>
                    )}
                    <Card label="Repository Usage by Hour" to={`/metering${singleSpace ? `?customer_id=${encodeURIComponent(singleSpace)}` : ""}`}>
                      <p className="m-0 text-[13px] text-ink-2">Exact repository transfer and build time, hour by hour, and the storage the repository limit is enforced against.</p>
                    </Card>
                  </>
                }
              >
                {shown && (
                  <Card
                    label="Over Time"
                    title={shown.title}
                    actions={<StateTabs label="Stack by" value={stackBy} onChange={setStackBy} tabs={[{ value: "workspace", label: "By workspace" }, { value: "kind", label: "By kind" }]} />}
                  >
                    <DimensionChips reading={reading} value={shown.dimension} onChange={(d) => setParam("dimension", d === "storage" ? null : d)} />
                    <ChartFor reading={reading} dimension={shown} by={stackBy} names={names} />
                  </Card>
                )}
                {shown && <BreakdownCard dimension={shown} names={names} />}
              </Split>
              <WorkspaceTable reading={reading} names={names} />
            </>
          )}
        </>
      )}
      {limitOpen && <SetLimit customerId={limitOpen} limits={limits.find((q) => q.data?.customer_id === limitOpen)?.data ?? null} reading={reading} onClose={() => setLimitOpen(null)} />}
    </Page>
  );
}

function Figures({ reading, before, previousLabel, selected, hrefFor }: { reading: Reading; before: Reading | null; previousLabel: string; selected: string; hrefFor: (d: string) => string }) {
  return (
    <div className="mb-5 grid grid-cols-2 gap-3 lg:grid-cols-3 xl:grid-cols-5">
      {reading.dimensions.map((d) => {
        const prior = before?.dimensions.find((x) => x.dimension === d.dimension)?.total;
        const moved = change(d.total, prior);
        const Arrow = moved.direction === "up" ? ArrowUp : moved.direction === "down" ? ArrowDown : null;
        return (
          <div key={d.dimension} className={cx("rounded-[6px]", selected === d.dimension && "ring-1 ring-action")} aria-current={selected === d.dimension ? "true" : undefined}>
            <Stat
              label={d.title}
              value={formatValue(d.total, d.unit)}
              to={hrefFor(d.dimension)}
              sub={
                <span className="inline-flex items-center gap-1">
                  {Arrow && <Arrow size={12} aria-hidden />}
                  {prior === undefined ? (d.aggregation === "last" ? "latest reading" : "this period") : `${moved.text} vs ${previousLabel}`}
                </span>
              }
            />
          </div>
        );
      })}
    </div>
  );
}

function DimensionChips({ reading, value, onChange }: { reading: Reading; value: string; onChange: (d: string) => void }) {
  return (
    <div className="mb-3 flex flex-wrap gap-1.5">
      {reading.dimensions.map((d) => (
        <Chip key={d.dimension} onClick={() => onChange(d.dimension)} active={d.dimension === value}>
          {d.title}
        </Chip>
      ))}
    </div>
  );
}

function ChartFor({ reading, dimension, by, names }: { reading: Reading; dimension: Dimension; by: "workspace" | "kind"; names: Record<string, string> }) {
  const series = useMemo(() => stackSeries(dimension, by, names), [dimension, by, names]);
  return <UsageChart periods={reading.periods} labels={series.labels} values={series.values} stacked={series.stacked} unit={dimension.unit} title={dimension.title} />;
}

function BreakdownCard({ dimension, names }: { dimension: Dimension; names: Record<string, string> }) {
  const lists: [string, Row[] | undefined][] = [
    ["By Kind", dimension.by_kind],
    ["By Agent", dimension.by_agent],
    ["By Run", dimension.by_launch],
    ["By Model", dimension.by_model],
    ["By Workload or Model", dimension.by_object],
    ["By GPU Class", dimension.by_class],
  ];
  const total = dimension.total || 1;
  return (
    <>
      {lists
        .filter(([, rows]) => rows && rows.length)
        .map(([title, rows]) => (
          <Card key={title} label={title} padded={false}>
            <DataTable
              rows={rows as Row[]}
              rowKey={(r) => String(r.key)}
              columns={[
                {
                  key: "name",
                  label: "Name",
                  sort: (r) => breakdownName(r as never, names),
                  render: (r) => (
                    <div className="min-w-0">
                      <div className="font-semibold text-ink">{breakdownName(r as never, names)}</div>
                      {(r.kind || r.agent) && <div className="text-[12px] text-ink-3">{r.kind || r.agent}</div>}
                    </div>
                  ),
                },
                { key: "total", label: dimension.aggregation === "last" ? "Latest" : "Total", numeric: true, sort: (r) => Number(r.total), render: (r) => <span className="font-mono">{formatValue(Number(r.total), dimension.unit)}</span> },
                { key: "share", label: "Share", numeric: true, render: (r) => <span className="font-mono text-ink-3">{Math.round((Number(r.total) / total) * 100)}%</span> },
              ]}
            />
          </Card>
        ))}
    </>
  );
}

function WorkspaceTable({ reading, names }: { reading: Reading; names: Record<string, string> }) {
  const rows = reading.workspaces.map((w) => ({ ...w, name: names[w.workspace_id] ?? w.name }));
  const value = (d: Dimension, id: string) => d.by_workspace.find((b) => b.key === id)?.total ?? 0;
  return (
    <Card label="By Workspace" padded={false} className="mt-4">
      <DataTable
        rows={rows}
        rowKey={(w) => w.workspace_id}
        empty={<p className="muted m-0 p-4 text-[13px]">No workspace was read.</p>}
        columns={[
          {
            key: "workspace",
            label: "Workspace",
            sort: (w) => w.name,
            render: (w) => (
              <div className="min-w-0">
                <div className="font-semibold text-ink">{w.name}</div>
                {!w.measured && <div className="text-[12px] text-bad">Not read: {w.message}</div>}
              </div>
            ),
          },
          ...reading.dimensions.map((d) => ({
            key: d.dimension,
            label: d.title,
            numeric: true,
            sort: (w: (typeof rows)[number]) => value(d, w.workspace_id),
            render: (w: (typeof rows)[number]) => <span className="font-mono">{formatValue(value(d, w.workspace_id), d.unit)}</span>,
          })),
        ]}
      />
    </Card>
  );
}

function LimitsCard({ limits, spaceNames, onSet }: { limits: Limits[]; spaceNames: Record<string, string>; onSet: (customerId: string) => void }) {
  const rows = limits.flatMap((l) => l.limits.map((limit) => ({ ...limit, customer_id: l.customer_id })));
  return (
    <Card label="Limits" title={rows.length ? `${rows.length} set` : "None set"} actions={limits.length === 1 ? <Button variant="ghost" onClick={() => onSet(limits[0].customer_id)}>Set a limit</Button> : undefined}>
      {rows.length === 0 ? (
        <p className="m-0 text-[13px] text-ink-2">No limits yet. A limit warns from a percentage you choose; the storage limit also refuses new repository writes above it, and the model tokens limit refuses agents' model calls.</p>
      ) : (
        <div className="space-y-3">
          {rows.map((l) => {
            const tone = limitTone(l.state);
            return (
              <div key={`${l.customer_id}:${l.dimension}`}>
                <div className="mb-1 flex items-baseline justify-between gap-3 text-[13px]">
                  <span className="text-ink-2">
                    {l.title}
                    {limits.length > 1 && <span className="text-ink-3"> · {spaceNames[l.customer_id] ?? "account"}</span>}
                  </span>
                  <span className={cx("font-mono", tone === "bad" ? "text-bad" : tone === "warn" ? "text-warn" : "text-ink")}>
                    {formatValue(l.used, l.unit)} of {formatValue(l.limit, l.unit)}
                  </span>
                </div>
                <Meter value={l.used ?? 0} max={l.limit} tone={tone === "bad" ? "bad" : undefined} label={l.percent === null ? "—" : `${Math.round(l.percent)}%`} />
                <div className="mt-1 text-[12px] text-ink-3">
                  {limitNote(l)}
                  {l.state === "warning" && " · near the limit"}
                  {l.state === "over" && " · over the limit"}
                </div>
              </div>
            );
          })}
        </div>
      )}
    </Card>
  );
}

function SetLimit({ customerId, limits, reading, onClose }: { customerId: string; limits: Limits | null; reading: Reading | null; onClose: () => void }) {
  const toast = useToast();
  const dimensions = reading?.dimensions ?? [];
  const [dimension, setDimension] = useState(dimensions[0]?.dimension ?? "storage");
  const unit = dimensions.find((d) => d.dimension === dimension)?.unit ?? "bytes";
  const existing = limits?.limits.find((l) => l.dimension === dimension);
  const [amount, setAmount] = useState(existing ? fromLimit(existing.limit, unit) : "");
  const [warn, setWarn] = useState(String(existing?.warn_percent ?? 80));
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const scale = limitScale(unit);
  const choose = (next: string) => {
    setDimension(next);
    const nextUnit = dimensions.find((d) => d.dimension === next)?.unit ?? "bytes";
    const found = limits?.limits.find((l) => l.dimension === next);
    setAmount(found ? fromLimit(found.limit, nextUnit) : "");
    setWarn(String(found?.warn_percent ?? 80));
  };
  const submit = async () => {
    setError(null);
    const limit = amount.trim() === "" || amount.trim() === "0" ? 0 : toLimit(amount, unit);
    if (limit === null) {
      setError(new ApiError(400, "invalid_input", `the limit must be a number of ${scale.label}`, "/limit"));
      return;
    }
    const percent = Number(warn);
    if (!Number.isInteger(percent) || percent < 1 || percent > 100) {
      setError(new ApiError(400, "invalid_input", "warn from a whole percentage between 1 and 100", "/warn_percent"));
      return;
    }
    setBusy(true);
    try {
      await act("set_usage_limit", { customer_id: customerId, dimension, limit, warn_percent: percent });
      toast(limit === 0 ? "Limit removed" : "Limit set");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const limitError = fieldError(error, "limit");
  const warnError = fieldError(error, "warn_percent");
  return (
    <Modal
      title="Set a Usage Limit"
      subtitle="A limit warns as usage nears it. Two are enforced: above the storage limit new repository writes are refused, and existing content stays readable; at the model tokens limit agents' model calls are refused until the month ends."
      onClose={onClose}
      width={520}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="usage-limit" loading={busy}>
            Save limit
          </Button>
        </>
      }
    >
      <Form id="usage-limit" onSubmit={submit} error={limitError || warnError ? null : error}>
        <Field label="What to limit" required>
          <select value={dimension} onChange={(e) => choose(e.target.value)}>
            {dimensions.map((d) => (
              <option key={d.dimension} value={d.dimension}>
                {d.title}
              </option>
            ))}
          </select>
        </Field>
        <Field label={`Limit in ${scale.label}${dimension === "storage" ? "" : " a month"}`} error={limitError} help="Leave empty or 0 to remove the limit.">
          <input inputMode="decimal" value={amount} onChange={(e) => setAmount(e.target.value)} autoFocus />
        </Field>
        <Field label="Warn from (percent of the limit)" error={warnError}>
          <input type="number" min={1} max={100} value={warn} onChange={(e) => setWarn(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

