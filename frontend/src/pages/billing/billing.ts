/** Billing (lake#564, decision 56): the shapes the billing capabilities answer and the pure rules the
 * pages share — amounts in a currency from millionths, a price in a person's units, the months a bill
 * covers, what a plan change means, and an invoice's state as a chip. No React here, so `npm test`
 * runs it (tests/billing.test.ts). Billing exists only where the lake has it on (`viewer.billing`). */
import { formatValue } from "../usage/usage.ts";

export type Price = { dimension: string; title: string; unit: string; unit_micros: number; per: number; included: number };
export type Plan = { id: string; name: string; description: string; currency: string; base_micros: number; active: boolean; position: number; prices: Price[]; updated_at?: string };
export type Subscription = { effective_from: string; plan_id: string; plan: Plan | null };
export type Account = {
  customer_id: string;
  month: string;
  current: Subscription | null;
  next: Subscription | null;
  history: { effective_from: string; plan_id: string }[];
  provider: { name: string; title: string; records_payments_by_hand: boolean };
};
export type Line = { kind: "plan" | "usage"; dimension: string; description: string; unit: string; quantity: number; included: number; billable: number; unit_micros: number; per: number; amount_micros: number };
export type Invoice = {
  id: string;
  number: number;
  customer_id: string;
  period: string;
  plan_id: string;
  plan_name: string;
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
  lines: Line[];
};
export type Bill = {
  customer_id: string;
  period: string;
  plan: { id: string; name: string };
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

/** A price as a person reads it: "$1.00 per 1,000 calls", "$0.10 per 1 GiB". */
export function priceLabel(price: Pick<Price, "unit_micros" | "per" | "unit">, currency: string): string {
  return `${money(price.unit_micros, currency)} per ${formatValue(price.per, price.unit)}`;
}

/** What a plan includes of one dimension each month: "1,000 calls included", or nothing. */
export function includedLabel(price: Pick<Price, "included" | "unit">): string {
  return price.included > 0 ? `${formatValue(price.included, price.unit)} included` : "";
}

/** A quantity in its dimension's unit ("3.5 GiB", "2,500 calls"); a plan line is its month. */
export function quantityLabel(line: Pick<Line, "kind" | "quantity" | "unit">): string {
  return line.kind === "plan" ? "1 month" : formatValue(line.quantity, line.unit);
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

/** What choosing `planId` does for an account: nothing (already on it with no change pending), start
 * now (no plan yet), cancel a pending change, or change from next month. */
export function choiceOf(account: Pick<Account, "current" | "next"> | null | undefined, planId: string): "current" | "start" | "cancel" | "next" {
  if (!account?.current) return "start";
  if (account.current.plan_id === planId) return account.next ? "cancel" : "current";
  return "next";
}

/** The sentence the confirmation shows for a choice. */
export function choiceSentence(choice: ReturnType<typeof choiceOf>, plan: string, nextMonth: string): string {
  switch (choice) {
    case "start":
      return `Start the ${plan} plan? It applies from the start of this month.`;
    case "cancel":
      return `Stay on the ${plan} plan? The change planned for ${monthLabel(nextMonth)} is cancelled.`;
    case "next":
      return `Switch to the ${plan} plan from ${monthLabel(nextMonth)}? This month stays on your current plan.`;
    default:
      return `You are on the ${plan} plan.`;
  }
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

/** A plan form's price row, in a person's units, back to a capability's input: a price per unit in the
 * currency ("1.00") to millionths, `per` and `included` as whole units. Null when a field is not a
 * number. */
export function priceInput(row: { dimension: string; price: string; per: string; included: string }): { dimension: string; unit_micros: number; per: number; included: number } | null {
  const price = Number(row.price);
  const per = Number(row.per || "1");
  const included = Number(row.included || "0");
  if (!row.dimension || !Number.isFinite(price) || price < 0 || !Number.isInteger(per) || per < 1 || !Number.isInteger(included) || included < 0) return null;
  return { dimension: row.dimension, unit_micros: Math.round(price * 1_000_000), per, included };
}

/** Whole currency units ("20", "19.99") to millionths; null when it is not an amount. */
export function microsOf(amount: string): number | null {
  const value = Number(amount);
  return amount.trim() !== "" && Number.isFinite(value) && value >= 0 ? Math.round(value * 1_000_000) : null;
}
