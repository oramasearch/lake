/** The price list (`/billing/plans`, lake#564, lake#635): the operator's editor for the instance's
 * one price list. Lake is priced by usage only: one rate per usage dimension, no plans, no monthly fee,
 * nothing included, and every account is billed at it. The editor shows the list in force this
 * month and any list published for a later month, publishes a new one (`set_price_list`) from a
 * chosen month, and lists every publication (`price_list` with `include_history`). A month is
 * billed at the list in force at its end. Only where the lake bills. */
import { useEffect, useState } from "react";
import { Tags, Upload } from "lucide-react";
import { act, ApiError, useRead } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { relative } from "@/lib/format";
import { Button, Card, DataTable, Empty, ErrorBlock, Field, Form, Page, Skeleton, Split } from "@/lib/ui";
import { DIMENSIONS, monthLabel, months, priceLabel, ratesInput, rateRows, rowTitle, type PriceList, type PriceListAnswer, type RateRow } from "./billing";

export default function PriceListPage() {
  useTrail([{ label: "Billing" }, { label: "Price List" }], "Price List");
  const answer = useRead<PriceListAnswer>("price_list", { include_history: true }, { workspace: null });
  const current = answer.data?.list ?? null;
  return (
    <Page
      eyebrow="Administration"
      title="Price List"
      sub="One rate per kind of usage, and one per GPU class. Compute is priced as the CPU, RAM, GPU and GPU memory each workload is scheduled with. Every account pays for what it uses at these rates: no plans, no monthly fee, nothing included."
      wide
    >
      {answer.isError ? (
        <ErrorBlock error={answer.error} retry={() => void answer.refetch()} what="the price list" />
      ) : !answer.data ? (
        <Skeleton lines={8} />
      ) : (
        <Split side={<History lists={answer.data.history ?? []} current={current?.id} next={answer.data.next?.id} />}>
          <Editor current={current} next={answer.data.next} />
        </Split>
      )}
    </Page>
  );
}

function Editor({ current, next }: { current: PriceList | null; next: PriceList | null }) {
  const toast = useToast();
  const [rows, setRows] = useState<RateRow[]>(() => rateRows(current?.prices ?? null));
  const [effective, setEffective] = useState(months().current);
  const [currency, setCurrency] = useState(current?.currency ?? "USD");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  // A newly published list (this page's or another operator's) becomes the rows to edit.
  useEffect(() => {
    setRows(rateRows(current?.prices ?? null));
  }, [current?.id]);
  const update = (index: number, change: Partial<RateRow>) => setRows(rows.map((r, i) => (i === index ? { ...r, ...change } : r)));
  const submit = async () => {
    setError(null);
    const input = ratesInput(rows);
    if ("bad" in input) {
      setError(new ApiError(400, "invalid_input", `${rowTitle(rows[input.bad])}: a rate is an amount, such as 0.04, and "per" a whole number above 0`, `/prices/${input.bad}`));
      return;
    }
    setBusy(true);
    try {
      const published = await act<PriceList>("set_price_list", { currency, effective_from: effective, prices: input.prices });
      toast(`Price list published from ${monthLabel(published.effective_from)}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Card
      label={current ? `In Force Since ${monthLabel(current.effective_from)}` : "Not Published Yet"}
      title="Rates"
      footer={
        <span>
          A list applies from the first day of its month. A month is billed at the list in force at its end, so a list published for this month prices all of it. Issued invoices keep their rates.
          {next ? ` A list is already published for ${monthLabel(next.effective_from)}.` : ""}
        </span>
      }
    >
      {!current && (
        <p className="m-0 mb-3 text-[13px] text-ink-2">
          Nothing is published on this Lake yet. The rows start from example rates; set your own and publish them to start billing.
        </p>
      )}
      <Form id="price-list" onSubmit={submit} error={error}>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <Field label="From month" required help="The list applies from the first day of this UTC month.">
            <input type="month" value={effective} onChange={(e) => setEffective(e.target.value)} />
          </Field>
          <Field label="Currency" required help="Card payment through Stripe bills USD only.">
            <input value={currency} maxLength={3} onChange={(e) => setCurrency(e.target.value.toUpperCase())} />
          </Field>
        </div>
        {rows.map((row, index) => {
          const dimension = DIMENSIONS.find((d) => d.name === row.dimension)!;
          const per = Number(row.per);
          const rate = Number(row.rate);
          const preview = row.rate.trim() === "" ? "not charged" : Number.isFinite(rate) && Number.isInteger(per) && per > 0 ? priceLabel({ unit_micros: Math.round(rate * 1_000_000), per, unit: dimension.unit }, currency) : "";
          return (
            <div key={`${row.dimension}:${row.class}`} className="grid grid-cols-2 items-end gap-2 sm:grid-cols-[2fr_1fr_1fr_2fr]">
              <div className={`col-span-2 self-center text-[13px] text-ink sm:col-span-1 ${row.class ? "pl-3" : "font-semibold"}`}>{rowTitle(row)}</div>
              <Field label={`Rate (${currency})`}>
                <input inputMode="decimal" value={row.rate} placeholder="not charged" onChange={(e) => update(index, { rate: e.target.value })} />
              </Field>
              <Field label={`Per (${UNIT_WORDS[dimension.unit] ?? dimension.unit.replace(/_/g, " ")})`}>
                <input inputMode="numeric" value={row.per} onChange={(e) => update(index, { per: e.target.value })} />
              </Field>
              <div className="col-span-2 self-center text-[12.5px] text-ink-3 sm:col-span-1">{preview}</div>
            </div>
          );
        })}
        <div className="mt-2 flex flex-wrap gap-2">
          <Button variant="primary" type="submit" loading={busy}>
            <Upload size={14} aria-hidden /> Publish from {monthLabel(effective)}
          </Button>
          <Button onClick={() => setRows(rateRows(null))}>Reset to the list rates</Button>
        </div>
      </Form>
    </Card>
  );
}

/** What a rate's "per" counts, in words, for the units a person would not guess. */
const UNIT_WORDS: Record<string, string> = {
  mcpu_seconds: "thousandths of a vCPU × seconds",
  mib_seconds: "MiB × seconds",
  gpu_seconds: "GPU × seconds",
  gib_seconds: "GiB × seconds",
};

function History({ lists, current, next }: { lists: PriceList[]; current?: string; next?: string }) {
  return (
    <Card label="Publications" padded={false}>
      <DataTable
        rows={lists}
        rowKey={(l) => l.id}
        empty={
          <div className="p-4">
            <Empty icon={Tags} title="Nothing published yet">
              Each publication is kept here.
            </Empty>
          </div>
        }
        columns={[
          {
            key: "from",
            label: "From",
            render: (l) => (
              <div className="min-w-0">
                <div className="font-semibold text-ink">
                  {monthLabel(l.effective_from)}
                  {l.id === current ? " · in force" : l.id === next ? " · next" : ""}
                </div>
                <div className="text-[12px] text-ink-3">{l.published_at ? `published ${relative(l.published_at)}` : l.id}</div>
              </div>
            ),
          },
          { key: "rates", label: "Rates", numeric: true, render: (l) => <span className="font-mono">{l.prices.length}</span> },
        ]}
      />
    </Card>
  );
}
