/** Billing (lake#564, decision 56): the shapes the billing capabilities answer and the pure rules the
 * pages share — amounts in a currency from millionths, a rate in a person's units, a line as usage ×
 * rate = cost, the months a bill covers, the price list editor's rows, and an invoice's state as a
 * chip. Lake is priced by usage only (lake#635): one rate per usage dimension, no plans, no fee,
 * nothing included. No React here, so `npm test` runs it (tests/billing.test.ts). Billing exists
 * only where the lake has it on (`viewer.billing`). */
import { formatValue } from "../usage/usage.ts";

export type Price = { dimension: string; title: string; unit: string; unit_micros: number; per: number };
/** One publication of the price list: a rate per usage dimension, in force from `effective_from`. */
export type PriceList = { id: string; effective_from: string; published_at?: string; currency: string; prices: Price[] };
/** What `price_list` answers: the list in force for `period`, one published for a later month, and
 * (for the operator, with `include_history`) every publication, newest first. */
export type PriceListAnswer = { period: string; list: PriceList | null; next: PriceList | null; history?: PriceList[] };
export type Account = { customer_id: string; month: string; price_list: PriceList | null; provider: Provider };
/** How invoices are paid (lake#620): Stripe, or invoice only with `reason` saying why card payment is
 * not set up. */
export type Provider = { name: string; title: string; records_payments_by_hand: boolean; online_payment?: boolean; reason?: string };
export type Line = { kind: "usage"; dimension: string; description: string; unit: string; quantity: number; included: number; billable: number; unit_micros: number; per: number; amount_micros: number };
export type Invoice = {
  id: string;
  number: number;
  customer_id: string;
  period: string;
  /** The price list publication the invoice was billed at; its rates are frozen on the lines. */
  price_list_id: string;
  currency: string;
  total_micros: number;
  state: "due" | "paid" | "void";
  provider: string;
  provider_reference: string;
  instructions: string;
  note: string;
  issued_at: string;
  paid_at: string;
  voided_at: string;
  /** Where the account pays (Stripe's hosted invoice page); empty for an invoice paid outside lake. */
  payment_url?: string;
  /** The provider's own state of the invoice: open, paid, payment_failed, void. */
  provider_status?: string;
  lines: Line[];
};
export type Bill = {
  customer_id: string;
  period: string;
  price_list: { id: string; effective_from: string };
  currency: string;
  lines: Line[];
  total_micros: number;
  complete: boolean;
  unread_workspaces?: { workspace_id: string; name: string; message: string }[];
  not_measured: string[];
  invoice?: Invoice | null;
};

/** An amount of `micros` millionths of `currency` ("USD"), to the cent; an amount under one unit keeps
 * its fractions of a cent (a price of $0.0004 a call). */
export function money(micros: number, currency: string): string {
  const units = micros / 1_000_000;
  const cents = Math.round(units * 100) / 100;
  const digits = Math.abs(units) < 1 && Math.abs(units - cents) > 1e-9 ? Math.min(6, Math.max(2, decimals(units))) : 2;
  try {
    return new Intl.NumberFormat("en-US", { style: "currency", currency, minimumFractionDigits: digits, maximumFractionDigits: digits }).format(units);
  } catch {
    return `${units.toFixed(digits)} ${currency}`;
  }
}

function decimals(value: number): number {
  const text = value.toFixed(6).replace(/0+$/, "");
  const dot = text.indexOf(".");
  return dot < 0 ? 0 : text.length - dot - 1;
}

/** What a rate counts, as a person reads it: time by the minute or hour ("minute",
 * "instance-minute"), a volume by the GiB-month, anything else in its unit ("1,000 calls", "1 GiB"). */
export function perLabel(per: number, unit: string): string {
  if (unit === "seconds" && (per === 60 || per === 3600)) return per === 60 ? "minute" : "hour";
  if (unit === "instance_seconds" && (per === 60 || per === 3600)) return per === 60 ? "instance-minute" : "instance-hour";
  if (unit === "gib_seconds" && per === 2_628_000) return "GiB-month";
  return formatValue(per, unit);
}

/** A price as a person reads it: "$1.00 per 1,000 calls", "$0.10 per 1 GiB", "$0.005 per minute". */
export function priceLabel(price: Pick<Price, "unit_micros" | "per" | "unit">, currency: string): string {
  return `${money(price.unit_micros, currency)} per ${perLabel(price.per, price.unit)}`;
}

/** A quantity in its dimension's unit ("3.5 GiB", "2,500 calls"). */
export function quantityLabel(line: Pick<Line, "quantity" | "unit">): string {
  return formatValue(line.quantity, line.unit);
}

/** What a line bills when it is not what was used: time is billed by the minute on the month's
 * total (lake#637), so 100 seconds are "billed as 2 min". Empty otherwise. */
export function billedNote(line: Pick<Line, "quantity" | "billable" | "unit">): string {
  return line.billable !== line.quantity ? `billed as ${formatValue(line.billable, line.unit)}` : "";
}

/** A line as usage × rate = cost, on what is billed: "3,500 calls × $1.00 per 1,000 calls = $3.50";
 * a dimension with no rate is "not charged". */
export function lineSentence(line: Pick<Line, "quantity" | "billable" | "unit" | "unit_micros" | "per" | "amount_micros">, currency: string): string {
  if (!line.unit_micros) return `${quantityLabel(line)}, not charged`;
  return `${quantityLabel({ quantity: line.billable, unit: line.unit })} × ${priceLabel(line, currency)} = ${money(line.amount_micros, currency)}`;
}

/** "2026-10" as "October 2026". */
export function monthLabel(period: string): string {
  const match = /^(\d{4})-(\d{2})/.exec(period);
  if (!match) return period;
  const date = new Date(Date.UTC(Number(match[1]), Number(match[2]) - 1, 1));
  return date.toLocaleString("en-US", { month: "long", year: "numeric", timeZone: "UTC" });
}

/** This UTC month and the one before, as "YYYY-MM". */
export function months(now: Date = new Date()): { current: string; previous: string } {
  const year = now.getUTCFullYear();
  const month = now.getUTCMonth();
  const name = (y: number, m: number) => `${y}-${String(m + 1).padStart(2, "0")}`;
  return { current: name(year, month), previous: month === 0 ? name(year - 1, 11) : name(year, month - 1) };
}

/** An invoice's state as a chip tone. */
export function invoiceTone(state: string): "warning" | "success" | "neutral" {
  return state === "due" ? "warning" : state === "paid" ? "success" : "neutral";
}

/** What an account owes: the total of its due invoices, by currency. */
export function owed(invoices: Pick<Invoice, "state" | "total_micros" | "currency">[]): { currency: string; micros: number }[] {
  const by = new Map<string, number>();
  for (const invoice of invoices) {
    if (invoice.state === "due") by.set(invoice.currency, (by.get(invoice.currency) ?? 0) + invoice.total_micros);
  }
  return [...by.entries()].map(([currency, micros]) => ({ currency, micros }));
}

/** The usage dimensions a rate can be set for, as usage_over_time names them (docs/api/metering.md). */
export const DIMENSIONS: { name: string; title: string; unit: string }[] = [
  { name: "storage", title: "Storage", unit: "bytes" },
  { name: "transfer", title: "Transfer", unit: "bytes" },
  { name: "build_compute", title: "Build Compute", unit: "seconds" },
  { name: "run_compute", title: "Workspace Run Compute", unit: "seconds" },
  { name: "speech", title: "Speech to Text", unit: "seconds" },
  { name: "calls", title: "API and MCP Calls", unit: "calls" },
  { name: "predictions", title: "Model Predictions", unit: "calls" },
  { name: "model_tokens", title: "Model Tokens", unit: "tokens" },
  { name: "instances", title: "Service and App Instances", unit: "instance_seconds" },
  { name: "volumes", title: "Service and Store Volumes", unit: "gib_seconds" },
];

const GIB = 1024 ** 3;

/** Orama Cloud's list rates (lake#623, lake#635, lake#637; docs/api/billing.md#price-list), what the
 * editor starts from when nothing is published: millionths of a dollar for every `per` units, time
 * per minute. */
export const LIST_RATES: { dimension: string; unit_micros: number; per: number }[] = [
  { dimension: "storage", unit_micros: 40_000, per: GIB },
  { dimension: "transfer", unit_micros: 50_000, per: GIB },
  { dimension: "build_compute", unit_micros: 5_000, per: 60 },
  { dimension: "run_compute", unit_micros: 4_000, per: 60 },
  { dimension: "speech", unit_micros: 7_200, per: 60 },
  { dimension: "calls", unit_micros: 2_000_000, per: 1_000_000 },
  { dimension: "predictions", unit_micros: 10_000, per: 1000 },
  { dimension: "model_tokens", unit_micros: 9_000_000, per: 1_000_000 },
  { dimension: "instances", unit_micros: 1_350, per: 60 },
  { dimension: "volumes", unit_micros: 200_000, per: 2_628_000 },
];

/** One row of the price list editor, in a person's numbers: the rate in the currency ("0.04") for
 * every `per` units; an empty rate leaves the dimension uncharged. */
export type RateRow = { dimension: string; rate: string; per: string };

/** The editor's rows, one per dimension, from a published list (or the list rates when there is
 * none). A dimension the list does not price starts empty. */
export function rateRows(prices: Pick<Price, "dimension" | "unit_micros" | "per">[] | null | undefined): RateRow[] {
  const from = prices ?? LIST_RATES;
  return DIMENSIONS.map((d) => {
    const price = from.find((p) => p.dimension === d.name);
    return { dimension: d.name, rate: price ? String(price.unit_micros / 1_000_000) : "", per: price ? String(price.per) : "1" };
  });
}

/** The editor's rows back to `set_price_list`'s prices: the rate to millionths, `per` a whole number
 * above 0; a row without a rate is left out. `{bad}`, the row's index, when a row is not a number. */
export function ratesInput(rows: RateRow[]): { prices: { dimension: string; unit_micros: number; per: number }[] } | { bad: number } {
  const prices: { dimension: string; unit_micros: number; per: number }[] = [];
  for (let index = 0; index < rows.length; index++) {
    const row = rows[index];
    if (row.rate.trim() === "") continue;
    const rate = Number(row.rate);
    const per = Number(row.per || "1");
    if (!row.dimension || !Number.isFinite(rate) || rate < 0 || !Number.isInteger(per) || per < 1) return { bad: index };
    prices.push({ dimension: row.dimension, unit_micros: Math.round(rate * 1_000_000), per });
  }
  return { prices };
}

/** Whether a person can pay `invoice` on its provider's page (lake#620): it is due and has an HTTPS
 * payment page. */
export function payOnline(invoice: Pick<Invoice, "state" | "payment_url">): string | null {
  const url = invoice.payment_url ?? "";
  return invoice.state === "due" && /^https:\/\/[^\s]+$/.test(url) ? url : null;
}

/** Whether an operator records `invoice`'s payment by hand: only an invoice issued by the manual
 * provider. A Stripe invoice is paid through Stripe. */
export function recordedByHand(invoice: Pick<Invoice, "provider">): boolean {
  return invoice.provider === "manual";
}

/** A provider's state of an invoice in words, and its chip tone. */
export function providerStatus(status: string | undefined): { label: string; tone: "warning" | "success" | "neutral" | "danger" } | null {
  switch (status ?? "") {
    case "":
      return null;
    case "open":
      return { label: "Awaiting payment", tone: "warning" };
    case "paid":
      return { label: "Paid", tone: "success" };
    case "payment_failed":
      return { label: "Payment failed", tone: "danger" };
    case "void":
      return { label: "Void", tone: "neutral" };
    case "draft":
      return { label: "Draft", tone: "neutral" };
    default:
      return { label: status!.replace(/_/g, " "), tone: "neutral" };
  }
}

/** The Payment figure on the Billing page: the provider and how an invoice is paid. */
export function paymentSummary(provider: Provider): { value: string; sub: string } {
  if (!provider.records_payments_by_hand) return { value: provider.title, sub: `pay each invoice on ${provider.title}'s page` };
  return { value: provider.title, sub: provider.reason ? "card payment is not set up" : "Orama records each payment" };
}
