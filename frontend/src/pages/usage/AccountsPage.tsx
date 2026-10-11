/** Usage → every account (`/usage/accounts`, lake#564): the operator's view across customers, for
 * capacity planning. Each account's usage this month (or the range chosen) from `usage_over_time`
 * over all its workspaces, the totals across accounts, and a link to each account's own Usage
 * (`/usage?customer_id=`). At most the first 200 accounts are read, each with one call. */
import { useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { Users } from "lucide-react";
import { read, useList, type Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Banner, Button, DataTable, Page, SearchInput, StateTabs, Stat, StatRow, Toolbar } from "@/lib/ui";
import { RANGES, formatValue, mergeReadings, rangeOf, usageLink, windows, type RangeKey, type Reading } from "./usage";

const MAX_ACCOUNTS = 200;
const FIGURES = ["storage", "calls", "cpu", "ram", "gpu", "volumes", "model_tokens"];

export default function AccountsPage() {
  useTrail([{ label: "Usage", to: "/usage" }, { label: "Every Account" }], "Every Account");
  const [params, setParams] = useSearchParams();
  const range = rangeOf(params.get("range"));
  const [search, setSearch] = useState("");
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  const span = useMemo(() => windows(range).current, [range]);
  const accounts = (customers.data ?? []).slice(0, MAX_ACCOUNTS);
  const readings = useQueries({
    queries: accounts.map((c) => ({
      queryKey: ["usage_over_time", c.id, null, span, "accounts"],
      queryFn: ({ signal }: { signal: AbortSignal }) =>
        read<Reading>("usage_over_time", { customer_id: c.id, from: span.from, to: span.to, granularity: span.granularity, dimensions: FIGURES }, { workspace: null, signal }),
      staleTime: 300_000,
    })),
  });
  const known = readings.map((q) => q.data).filter((r): r is Reading => !!r);
  const total = known.length ? mergeReadings(known) : null;
  const rows = accounts
    .map((c, i) => ({ customer: c, reading: readings[i]?.data, failed: readings[i]?.isError }))
    .filter((r) => !search || String(r.customer.name ?? "").toLowerCase().includes(search.toLowerCase()) || String(r.customer.id).includes(search));
  const figure = (reading: Reading | undefined, dimension: string) => reading?.dimensions.find((d) => d.dimension === dimension);
  return (
    <Page eyebrow="Administration" title="Every Account" sub="Each account's usage across all its workspaces, for capacity planning." wide>
      <StateTabs label="Period" value={range} onChange={(value: RangeKey) => setParams(value === "month" ? {} : { range: value }, { replace: true })} tabs={RANGES.map((r) => ({ value: r.key, label: r.label }))} />
      {(customers.data?.length ?? 0) > MAX_ACCOUNTS && <Banner tone="quiet">The first {MAX_ACCOUNTS} accounts are read; search the customer list for the rest.</Banner>}
      {total && (
        <StatRow cols={4}>
          <Stat label="Accounts" value={accounts.length} icon={Users} sub={`${known.length} read`} />
          {["storage", "cpu", "ram"].map((d) => {
            const f = figure(total, d);
            return <Stat key={d} label={f?.title ?? d} value={formatValue(f?.total ?? 0, f?.unit ?? "")} sub="every account" />;
          })}
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search accounts…" />
      </Toolbar>
      <DataTable
        q={customers}
        what="accounts"
        rows={rows}
        rowKey={(r) => String(r.customer.id)}
        initialSort={{ key: "storage", dir: "descending" }}
        columns={[
          {
            key: "account",
            label: "Account",
            sort: (r) => String(r.customer.name ?? ""),
            render: (r) => (
              <div className="min-w-0">
                <div className="font-semibold text-ink">{r.customer.name}</div>
                <div className="text-[12px] text-ink-3">
                  {r.customer.customer_type} · {r.failed ? "could not be read" : r.reading ? `${r.reading.workspaces.length} workspaces` : "reading…"}
                </div>
              </div>
            ),
          },
          ...FIGURES.map((d) => ({
            key: d,
            label: figure(total ?? undefined, d)?.title ?? d,
            numeric: true,
            sort: (r: (typeof rows)[number]) => figure(r.reading, d)?.total ?? -1,
            render: (r: (typeof rows)[number]) => {
              const f = figure(r.reading, d);
              return <span className="font-mono">{f ? formatValue(f.total, f.unit) : "—"}</span>;
            },
          })),
          {
            key: "open",
            label: "",
            render: (r) => (
              <Button variant="ghost" to={usageLink({ customerId: String(r.customer.id), range })}>
                Open
              </Button>
            ),
          },
        ]}
      />
    </Page>
  );
}
