/** Billing (lake#564, decision 56): the shapes the billing capabilities answer and the pure rules the
 * pages share — amounts in a currency from millionths, a rate in a person's units, a line as usage ×
 * rate = cost, the months a bill covers, the price list editor's rows, an invoice's state as a
 * chip, and the billing contact's countries and address (lake#857). Lake is priced by usage only (lake#635): one rate per usage dimension, no plans, no fee,
 * nothing included. No React here, so `npm test` runs it (tests/billing.test.ts). Billing exists
 * only where the lake has it on (`viewer.billing`). */
import { formatValue } from "../usage/usage.ts";

/** One rate of the price list. `class` is a GPU class (`l4`, `a10g`, `a100`, `h100`) for GPU and GPU
 * memory: a rate for that class alone; empty, the dimension's rate for every other class (lake#643). */
export type Price = { dimension: string; class?: string; title: string; unit: string; unit_micros: number; per: number; priced?: boolean };
/** One publication of the price list: a rate per usage dimension, in force from `effective_from`. */
export type PriceList = { id: string; effective_from: string; published_at?: string; currency: string; prices: Price[] };
/** What `price_list` answers: the list in force for `period`, one published for a later month, and
 * (for the operator, with `include_history`) every publication, newest first. */
export type PriceListAnswer = { period: string; list: PriceList | null; next: PriceList | null; history?: PriceList[] };
/** What `billing_account` answers. `suggested_email`, while there is no contact, is the signed-in
 * owner's or admin's own email to offer; nothing is stored until it is saved. */
export type Account = { customer_id: string; month: string; price_list: PriceList | null; provider: Provider; contact: Contact | null; suggested_email: string | null };
/** Where an account's invoices go, and the name and address on them (lake#857,
 * docs/api/billing.md#billing-contact). An empty `name` puts the account's name on the invoice;
 * `country` is an ISO 3166-1 alpha-2 code, or empty. */
export type Contact = { email: string; name: string; line1: string; line2: string; city: string; postal_code: string; state: string; country: string; updated_at: string };
/** The billing contact form's fields: what `set_billing_contact` takes besides `customer_id`. */
export type ContactDraft = Omit<Contact, "updated_at">;
/** What `set_billing_contact` answers: the saved contact, and whether Stripe's customer took it. */
export type ContactSaved = { customer_id: string; contact: Contact; provider_update: "updated" | "none" | "failed"; provider_error?: string };
/** How invoices are paid (lake#620): Stripe, or invoice only with `reason` saying why card payment is
 * not set up. */
export type Provider = { name: string; title: string; records_payments_by_hand: boolean; online_payment?: boolean; reason?: string };
export type Line = { kind: "usage"; dimension: string; class?: string; description: string; unit: string; quantity: number; included: number; billable: number; unit_micros: number; per: number; amount_micros: number };
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
  if (unit === "gpu_seconds" && (per === 60 || per === 3600)) return per === 60 ? "GPU-minute" : "GPU-hour";
  if (unit === "mcpu_seconds" && (per === 60_000 || per === 3_600_000)) return per === 60_000 ? "vCPU-minute" : "vCPU-hour";
  if (unit === "mib_seconds" && (per === 61_440 || per === 3_686_400)) return per === 61_440 ? "GiB-minute" : "GiB-hour";
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
 * total (lake#637), so 100 seconds are "billed as 2 min", and CPU, RAM, GPU and GPU memory by the
 * vCPU-, GiB- and GPU-minute (lake#643). Empty otherwise. */
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

/** The usage dimensions a rate can be set for, as usage_over_time names them (docs/api/metering.md).
 * Compute is CPU, RAM, GPU and GPU memory of what each workload is scheduled with (lake#643); build,
 * run and instance time are measured but no longer priced. `classes`: a rate per GPU class. */
export const DIMENSIONS: { name: string; title: string; unit: string; classes?: boolean }[] = [
  { name: "cpu", title: "CPU", unit: "mcpu_seconds" },
  { name: "ram", title: "RAM", unit: "mib_seconds" },
  { name: "gpu", title: "GPU", unit: "gpu_seconds", classes: true },
  { name: "vram", title: "GPU Memory", unit: "mib_seconds", classes: true },
  { name: "storage", title: "Storage", unit: "bytes" },
  { name: "transfer", title: "Transfer", unit: "bytes" },
  { name: "speech", title: "Speech to Text", unit: "seconds" },
  { name: "calls", title: "API and MCP Calls", unit: "calls" },
  { name: "predictions", title: "Model Predictions", unit: "calls" },
  { name: "searches", title: "Index Searches", unit: "calls" },
  { name: "model_tokens", title: "Model Tokens", unit: "tokens" },
  { name: "volumes", title: "Service and Store Volumes", unit: "gib_seconds" },
];

/** The GPU classes the editor offers a rate for; a published list may name others. */
export const GPU_CLASSES = ["l4", "a10g", "a100", "h100"];

/** A GPU class as a person reads it: "h100" is "H100". */
export function classTitle(name: string): string {
  return name.toUpperCase();
}

const GIB = 1024 ** 3;

/** Orama Cloud's list rates (lake#623, lake#635, lake#637, lake#643, lake#658;
 * docs/api/billing.md#price-list),
 * what the editor starts from when nothing is published: millionths of a dollar for every `per`
 * units. CPU per vCPU-minute (60,000 mCPU-seconds), RAM and GPU memory per GiB-minute (61,440
 * MiB-seconds), GPU per GPU-minute with a rate per class and a default for any other class. */
export const LIST_RATES: { dimension: string; class?: string; unit_micros: number; per: number }[] = [
  { dimension: "cpu", unit_micros: 1_500, per: 60_000 },
  { dimension: "ram", unit_micros: 170, per: 61_440 },
  { dimension: "gpu", unit_micros: 38_750, per: 60 },
  { dimension: "gpu", class: "l4", unit_micros: 10_800, per: 60 },
  { dimension: "gpu", class: "a10g", unit_micros: 21_500, per: 60 },
  { dimension: "gpu", class: "a100", unit_micros: 38_750, per: 60 },
  { dimension: "gpu", class: "h100", unit_micros: 78_750, per: 60 },
  { dimension: "vram", unit_micros: 250, per: 61_440 },
  { dimension: "storage", unit_micros: 40_000, per: GIB },
  { dimension: "transfer", unit_micros: 50_000, per: GIB },
  { dimension: "speech", unit_micros: 7_200, per: 60 },
  { dimension: "calls", unit_micros: 2_000_000, per: 1_000_000 },
  { dimension: "predictions", unit_micros: 10_000, per: 1000 },
  { dimension: "searches", unit_micros: 10_000, per: 1000 },
  { dimension: "model_tokens", unit_micros: 9_000_000, per: 1_000_000 },
  { dimension: "volumes", unit_micros: 200_000, per: 2_628_000 },
];

/** One row of the price list editor, in a person's numbers: the rate in the currency ("0.04") for
 * every `per` units; an empty rate leaves the dimension (or the class) uncharged. `class` is a GPU
 * class, empty for the dimension's own rate. */
export type RateRow = { dimension: string; class: string; rate: string; per: string };

/** A rate row's name for a person: "CPU", "GPU (H100)", "GPU, other classes". */
export function rowTitle(row: Pick<RateRow, "dimension" | "class">): string {
  const dimension = DIMENSIONS.find((d) => d.name === row.dimension);
  const title = dimension?.title ?? row.dimension;
  if (row.class) return `${title} (${classTitle(row.class)})`;
  return dimension?.classes ? `${title}, other classes` : title;
}

/** The editor's rows: one per dimension, and for GPU and GPU memory one more per GPU class (the
 * offered ones and any the list names), from a published list (or the list rates when there is
 * none). A dimension or class the list does not price starts empty. A rate for a dimension no
 * longer priced (instances, build or run time) is not carried over. */
export function rateRows(prices: Pick<Price, "dimension" | "class" | "unit_micros" | "per">[] | null | undefined): RateRow[] {
  const from = prices ?? LIST_RATES;
  const rows: RateRow[] = [];
  const row = (dimension: string, name: string): RateRow => {
    const price = from.find((p) => p.dimension === dimension && (p.class ?? "") === name);
    return { dimension, class: name, rate: price ? String(price.unit_micros / 1_000_000) : "", per: price ? String(price.per) : "1" };
  };
  for (const d of DIMENSIONS) {
    rows.push(row(d.name, ""));
    if (!d.classes) continue;
    const named = from.filter((p) => p.dimension === d.name && p.class).map((p) => p.class as string);
    for (const name of [...new Set([...GPU_CLASSES, ...named])]) rows.push(row(d.name, name));
  }
  return rows;
}

/** The editor's rows back to `set_price_list`'s prices: the rate to millionths, `per` a whole number
 * above 0, the class when the row has one; a row without a rate is left out. `{bad}`, the row's
 * index, when a row is not a number. */
export function ratesInput(rows: RateRow[]): { prices: { dimension: string; class?: string; unit_micros: number; per: number }[] } | { bad: number } {
  const prices: { dimension: string; class?: string; unit_micros: number; per: number }[] = [];
  for (let index = 0; index < rows.length; index++) {
    const row = rows[index];
    if (row.rate.trim() === "") continue;
    const rate = Number(row.rate);
    const per = Number(row.per || "1");
    if (!row.dimension || !Number.isFinite(rate) || rate < 0 || !Number.isInteger(per) || per < 1) return { bad: index };
    prices.push({ dimension: row.dimension, ...(row.class ? { class: row.class } : {}), unit_micros: Math.round(rate * 1_000_000), per });
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

/** The ISO 3166-1 alpha-2 codes officially assigned: what the contact form's country offers. */
const ISO_COUNTRIES = (
  "AD AE AF AG AI AL AM AO AQ AR AS AT AU AW AX AZ BA BB BD BE BF BG BH BI BJ BL BM BN BO BQ BR BS BT BV BW BY BZ " +
  "CA CC CD CF CG CH CI CK CL CM CN CO CR CU CV CW CX CY CZ DE DJ DK DM DO DZ EC EE EG EH ER ES ET FI FJ FK FM FO FR " +
  "GA GB GD GE GF GG GH GI GL GM GN GP GQ GR GS GT GU GW GY HK HM HN HR HT HU ID IE IL IM IN IO IQ IR IS IT JE JM JO JP " +
  "KE KG KH KI KM KN KP KR KW KY KZ LA LB LC LI LK LR LS LT LU LV LY MA MC MD ME MF MG MH MK ML MM MN MO MP MQ MR MS MT " +
  "MU MV MW MX MY MZ NA NC NE NF NG NI NL NO NP NR NU NZ OM PA PE PF PG PH PK PL PM PN PR PS PT PW PY QA RE RO RS RU RW " +
  "SA SB SC SD SE SG SH SI SJ SK SL SM SN SO SR SS ST SV SX SY SZ TC TD TF TG TH TJ TK TL TM TN TO TR TT TV TW TZ " +
  "UA UG UM US UY UZ VA VC VE VG VI VN VU WF WS YE YT ZA ZM ZW"
).split(" ");

/** Whether `code` is an assigned ISO 3166-1 alpha-2 code in capitals ("DE"). */
export function isCountryCode(code: string): boolean {
  return ISO_COUNTRIES.includes(code);
}

let regions: Intl.DisplayNames | undefined;

/** A country's English name ("DE" is "Germany"); a code the browser cannot name stays a code. */
export function countryName(code: string): string {
  try {
    regions ??= new Intl.DisplayNames(["en"], { type: "region" });
    return regions.of(code) ?? code;
  } catch {
    return code;
  }
}

/** The countries the contact form offers, by name. A saved code that is not an assigned one
 * (set_billing_contact takes any two capitals) is offered too, so saving the form keeps it. */
export function countryOptions(current = ""): { code: string; name: string }[] {
  const codes = current && !isCountryCode(current) ? [...ISO_COUNTRIES, current] : ISO_COUNTRIES;
  return codes.map((code) => ({ code, name: countryName(code) })).sort((a, b) => a.name.localeCompare(b.name, "en"));
}

/** The contact form, from the saved contact, else empty but for the email billing_account suggests. */
export function contactDraft(contact: Contact | null, suggested: string | null): ContactDraft {
  const from = contact ?? { email: suggested ?? "", name: "", line1: "", line2: "", city: "", postal_code: "", state: "", country: "" };
  return { email: from.email, name: from.name, line1: from.line1, line2: from.line2, city: from.city, postal_code: from.postal_code, state: from.state, country: from.country };
}

/** A contact's address as lines: the street, then city, state and postal code, then the country's
 * name. Empty parts are left out, so a contact with no address has no lines. */
export function addressLines(contact: Pick<Contact, "line1" | "line2" | "city" | "postal_code" | "state" | "country">): string[] {
  const place = [contact.city, contact.state, contact.postal_code].filter(Boolean).join(", ");
  return [contact.line1, contact.line2, place, contact.country ? countryName(contact.country) : ""].filter(Boolean);
}

/** Whether the Billing page asks for a billing email: the account has no contact, and its invoices
 * are paid online, so Stripe would email them once it has one. */
export function asksForBillingEmail(account: Pick<Account, "contact" | "provider">): boolean {
  return !account.contact && account.provider.online_payment === true;
}

/** What the page says after a save: the contact is kept even when the provider refuses it, and is
 * sent again at the next save. */
export function contactSavedMessage(saved: Pick<ContactSaved, "provider_update" | "provider_error">, provider: string): { text: string; tone: "ok" | "bad" } {
  if (saved.provider_update === "updated") return { text: `Billing contact saved and sent to ${provider}`, tone: "ok" };
  if (saved.provider_update === "failed") return { text: `Billing contact saved, but ${provider} refused it: ${saved.provider_error || "no reason given"}. It is sent again at the next save.`, tone: "bad" };
  return { text: "Billing contact saved", tone: "ok" };
}
