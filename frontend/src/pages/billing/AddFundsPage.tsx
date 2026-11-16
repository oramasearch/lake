// SPDX-License-Identifier: MIT

/** Add funds (`/billing/add-funds?customer_id=`, lake#1114, docs/ui/balance.md): the one page every
 * balance email, the banner and every 402 answer link to. It shows the account's balance and how much
 * to add, and starts a Stripe Checkout (`start_top_up`); Stripe sends the person back here with
 * `top_up=done` or `top_up=cancelled`, and the page reads the balance again until the credit shows.
 * The first top-up is the instance's first amount; later ones any amount from the minimum. Sign-in
 * first when needed (the router sends a visitor to /sign-in and back). */
import { useEffect, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { CreditCard, Wallet } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Card, Empty, ErrorBlock, Field, Form, Page, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
import { money } from "./billing";
import { balanceTone, dollarsToMicros, microsToDollars, returnNotice, topUpRule, type BalanceAnswer } from "./balance";

export default function AddFundsPage() {
  const session = useSession();
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") || session.viewer.customer_id || session.space?.id || "";
  const notice = returnNotice(params);
  const paidJustNow = params.get("top_up") === "done";
  useTrail([{ label: "Billing", to: "/billing" }, { label: "Add Funds" }], "Add Funds");
  const [startCredits, setStartCredits] = useState<number | null>(null);
  const balance = useRead<BalanceAnswer>(customerId ? "account_balance" : null, { customer_id: customerId }, {
    workspace: null,
    // Back from Stripe: read again until the credit arrives (a minute at most).
    refetchInterval: paidJustNow ? 3_000 : false,
  });
  const credited = paidJustNow && startCredits !== null && (balance.data?.credits.length ?? 0) > startCredits;
  useEffect(() => {
    if (paidJustNow && startCredits === null && balance.data) setStartCredits(balance.data.credits.length);
  }, [paidJustNow, startCredits, balance.data]);
  useEffect(() => {
    if (credited) session.refresh();
  }, [credited, session]);

  if (!customerId) {
    return (
      <Page eyebrow="Billing" title="Add Funds">
        <Empty icon={Wallet} title="Choose an account">
          Open Add funds from the account's Billing page.
        </Empty>
      </Page>
    );
  }
  return (
    <Page eyebrow="Billing" title="Add Funds" sub="Lake is prepaid: what you use is paid from your balance, at the published rates.">
      {notice && (
        <Banner tone={notice.tone} title={credited ? "Funds Added" : undefined}>
          {credited ? "Your balance is updated." : notice.text}
        </Banner>
      )}
      {balance.isError ? (
        <ErrorBlock error={balance.error} retry={() => void balance.refetch()} what="this account's balance" />
      ) : !balance.data ? (
        <Skeleton lines={5} />
      ) : (
        <AddFunds balance={balance.data} customerId={customerId} />
      )}
    </Page>
  );
}

function AddFunds({ balance, customerId }: { balance: BalanceAnswer; customerId: string }) {
  const rule = topUpRule(balance);
  const [amount, setAmount] = useState(microsToDollars(rule.amount));
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const micros = dollarsToMicros(amount);
  const problem = rule.problem(micros);
  const submit = async () => {
    if (problem || micros === null) return;
    setError(null);
    setBusy(true);
    try {
      const started = await act<{ checkout_url: string }>("start_top_up", { customer_id: customerId, amount_micros: micros });
      window.location.assign(started.checkout_url);
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  return (
    <>
      <StatRow cols={3}>
        <Stat label="Balance" value={money(balance.balance_micros, "USD")} sub={<Status value={balance.state} tone={balanceTone(balance.state)} />} />
        <Stat label="Limit" value={money(balance.limit_micros, "USD")} sub="Nothing that costs money starts below it" />
        <Stat label="Suggested" value={money(rule.amount, "USD")} sub={rule.first ? "Your first top-up" : "Brings your balance back up"} />
      </StatRow>
      <Card label="Add Funds" icon={<CreditCard size={14} aria-hidden />}>
        <Form onSubmit={submit} error={error}>
          <Field
            label="Amount in US dollars"
            required
            help={rule.first ? `Your first top-up is ${money(rule.amount, "USD")}. After it you can add any amount from ${money(rule.minimum, "USD")}.` : `Any amount from ${money(rule.minimum, "USD")}.`}
            error={amount && problem ? problem : undefined}
          >
            <input name="amount" inputMode="decimal" value={amount} readOnly={rule.first} onChange={(e) => setAmount(e.target.value)} />
          </Field>
          <p className="muted m-0 text-[13px]">You pay on Stripe's page. Lake never sees your card.</p>
          <div className="flex justify-end">
            <Button type="submit" variant="primary" loading={busy} disabled={!!problem}>
              <Wallet size={14} aria-hidden /> Add {micros !== null && !problem ? money(micros, "USD") : "funds"}
            </Button>
          </div>
        </Form>
      </Card>
    </>
  );
}
