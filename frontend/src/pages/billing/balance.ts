/** The prepaid balance (lake#1114, docs/api/billing.md#balance, docs/ui/balance.md): what
 * `account_balance` and `viewer.balance` answer, the banner every page shows while the balance is
 * low, negative or blocked, the Add funds page's link and amounts, and the history of credits and
 * charges. No React here, so `npm test` runs it (tests/balance.test.ts). */
import { money, monthLabel } from "./billing.ts";

export type BalanceState = "ok" | "low" | "negative" | "blocked";

export type AutoTopUp = {
  enabled: boolean;
  threshold_micros: number;
  amount_micros: number;
  card_saved: boolean;
  awaiting_card: boolean;
  in_flight: boolean;
  today: number;
  off_reason: string;
};

/** `viewer.balance` and the head of `account_balance`. */
export type Balance = {
  customer_id: string;
  currency: string;
  balance_micros: number;
  credits_micros?: number;
  charges_micros?: number;
  overdraft_micros: number;
  limit_micros: number;
  state: BalanceState;
  /** Why blocked: first_top_up or overdraft. */
  reason?: string;
  add_micros: number;
  add_funds_url: string;
  first_top_up_micros?: number;
  minimum_top_up_micros?: number;
  first_top_up_at?: string;
  /** Why it is never stopped (paid_order); empty otherwise. */
  exempt?: string;
  refreshed_at?: string;
  refresh_error?: string;
  auto_top_up?: AutoTopUp;
};

export type Credit = { id: string; kind: "top_up" | "auto_top_up"; amount_micros: number; provider: string; reference: string; created_at: string };
export type Charge = { period: string; amount_micros: number; price_list_id: string; invoice_id: string; refreshed_at: string };
export type BalanceAnswer = Balance & { credits: Credit[]; charges: Charge[] };

/** The console page that adds funds to an account; the banner, the 402 answers and the emails all
 * link to it. */
export function addFundsPath(customerId: string): string {
  return `/billing/add-funds?customer_id=${encodeURIComponent(customerId)}`;
}

/** The banner every page shows, or null while the balance is fine (or a paid order covers it). */
export function bannerFor(balance: Balance | null | undefined): { tone: "warn" | "info"; title: string; text: string } | null {
  if (!balance || balance.exempt || balance.state === "ok") return null;
  const now = money(balance.balance_micros, "USD");
  const add = money(balance.add_micros, "USD");
  const limit = money(balance.limit_micros, "USD");
  if (balance.state === "blocked" && balance.reason === "first_top_up") {
    return { tone: "info", title: "Add Funds to Start", text: `Add ${add} to your balance to start apps, runs, training, agents and model calls. Lake is prepaid: you pay for what you use from your balance.` };
  }
  if (balance.state === "blocked") {
    return { tone: "warn", title: "Paid Work Is Stopped", text: `Your balance is ${now}, at its ${limit} limit. Nothing that costs money starts until you add ${add}. What is running keeps running, and you can still read, stop and delete.` };
  }
  if (balance.state === "negative") {
    return { tone: "warn", title: "Your Balance Is Below $0", text: `Your balance is ${now}. At ${limit} Lake stops starting anything that costs money. Add ${add} to stay ahead.` };
  }
  return { tone: "info", title: "Your Balance Is Low", text: `Your balance is ${now}. Add ${add} so your work is not interrupted.` };
}

/** The state as a chip. */
export function balanceTone(state: BalanceState): "success" | "warning" | "danger" | "neutral" {
  return state === "ok" ? "success" : state === "low" ? "warning" : "danger";
}

/** Dollars a person typed ("25", "25.50", "$1,000") as millionths, or null when it is not an
 * amount of whole cents. */
export function dollarsToMicros(text: string): number | null {
  const clean = text.replace(/[$,\s]/g, "");
  if (!/^\d+(\.\d{1,2})?$/.test(clean)) return null;
  const [whole, cents = ""] = clean.split(".");
  return Number(whole) * 1_000_000 + Number(cents.padEnd(2, "0")) * 10_000;
}

/** Millionths as the dollars a field shows ("20", "25.50"). */
export function microsToDollars(micros: number): string {
  const cents = Math.round(micros / 10_000);
  return cents % 100 === 0 ? String(cents / 100) : (cents / 100).toFixed(2);
}

/** What a top-up must be: the first is exactly the first amount; later ones at least the minimum. */
export function topUpRule(balance: Balance): { first: boolean; amount: number; minimum: number; problem: (micros: number | null) => string | null } {
  const first = !balance.first_top_up_at;
  const firstAmount = balance.first_top_up_micros ?? 20_000_000;
  const minimum = balance.minimum_top_up_micros ?? 20_000_000;
  const amount = first ? firstAmount : Math.max(minimum, balance.add_micros);
  return {
    first,
    amount,
    minimum,
    problem: (micros) => {
      if (micros === null) return "Enter an amount in dollars and cents, such as 25 or 25.50.";
      if (first && micros !== firstAmount) return `Your first top-up is ${money(firstAmount, "USD")}.`;
      if (!first && micros < minimum) return `Add at least ${money(minimum, "USD")}.`;
      if (micros > 10_000_000_000) return "Add at most $10,000 at a time.";
      return null;
    },
  };
}

/** One row of the history: a credit (+) or a month's charge (−), newest first. */
export type HistoryRow = { key: string; when: string; label: string; detail: string; amount_micros: number };

export function historyRows(credits: Credit[], charges: Charge[]): HistoryRow[] {
  const rows: HistoryRow[] = [
    ...credits.map((c) => ({
      key: `credit-${c.id}`,
      when: c.created_at,
      label: c.kind === "auto_top_up" ? "Auto top-up" : "Top-up",
      detail: "Added by card",
      amount_micros: c.amount_micros,
    })),
    ...charges.map((c) => ({
      key: `charge-${c.period}`,
      when: `${c.period}-01T00:00:00Z`,
      label: `Usage, ${monthLabel(c.period)}`,
      detail: c.invoice_id ? "Invoiced" : "So far this month, refreshed every few minutes",
      amount_micros: -c.amount_micros,
    })),
  ];
  return rows.sort((a, b) => (a.when < b.when ? 1 : a.when > b.when ? -1 : 0));
}

/** What the Add funds page says after Stripe sends the person back. */
export function returnNotice(params: URLSearchParams): { tone: "ok" | "quiet"; text: string } | null {
  const done = params.get("top_up");
  if (done === "done") return { tone: "ok", text: "Payment received. Your balance updates as soon as Stripe confirms it, usually within a minute." };
  if (done === "cancelled") return { tone: "quiet", text: "You left the payment page. Nothing was charged." };
  const card = params.get("auto_top_up");
  if (card === "saved") return { tone: "ok", text: "Card saved. Auto top-up turns on as soon as Stripe confirms it." };
  if (card === "cancelled") return { tone: "quiet", text: "No card was saved, so auto top-up stays off." };
  return null;
}
