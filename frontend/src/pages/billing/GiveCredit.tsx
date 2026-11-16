// SPDX-License-Identifier: MIT

/** Give credit (lake#1134, docs/ui/balance.md#give-credit): the operator's form that gives an account
 * a credit from Orama through `grant_balance_credit`, by its ID or a person's email. Opened from an
 * account's Billing page and from Customers; the server refuses anyone but the operator or the
 * instance administrator. */
import { useState } from "react";
import { act } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, Field, Form } from "@/lib/ui";
import { money } from "./billing";
import { grantInput } from "./balance";

export function GiveCredit({ customerId, onClose, onGiven }: { customerId?: string; onClose: () => void; onGiven?: () => void }) {
  const toast = useToast();
  const [account, setAccount] = useState(customerId ?? "");
  const [dollars, setDollars] = useState("");
  const [reason, setReason] = useState("");
  const [expires, setExpires] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const { input, problem } = grantInput({ account, dollars, reason, expires });
  const submit = async () => {
    if (!input) return;
    setError(null);
    setBusy(true);
    try {
      const given = await act<{ emailed?: boolean; email_error?: string; balance: { balance_micros: number } }>("grant_balance_credit", input);
      toast(`Credit given: ${money(Number(input.amount_micros), "USD")}. The balance is ${money(given.balance.balance_micros, "USD")}${given.emailed ? ", and the account was emailed." : `. No email went${given.email_error ? ` (${given.email_error})` : ""}.`}`);
      onGiven?.();
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title="Give Credit"
      subtitle="A credit from Orama adds to the account's prepaid balance. It is spent before paid money, is not cash, and does not unlock the overdraft. The account is emailed."
      onClose={onClose}
      width={520}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="give-credit" loading={busy} disabled={!!problem}>
            Give credit
          </Button>
        </>
      }
    >
      <Form id="give-credit" onSubmit={submit} error={error}>
        {!customerId && (
          <Field label="Account ID or email" required help="A person's email gives the credit to their personal account.">
            <input name="account" value={account} onChange={(e) => setAccount(e.target.value)} autoComplete="off" />
          </Field>
        )}
        <Field label="Amount (USD)" required help="Up to $10,000.">
          <input name="amount" inputMode="decimal" value={dollars} onChange={(e) => setDollars(e.target.value)} />
        </Field>
        <Field label="Reason" required help="The account sees it on its Billing page.">
          <input name="reason" value={reason} onChange={(e) => setReason(e.target.value)} maxLength={1000} />
        </Field>
        <Field label="Last day it counts (UTC)" help="Optional. What is left at the end of that day stops counting; what it paid for stays paid.">
          <input name="expires" type="date" value={expires} onChange={(e) => setExpires(e.target.value)} />
        </Field>
        {problem && (account || dollars || reason) ? <p className="m-0 text-[12.5px] text-ink-3">{problem}</p> : null}
      </Form>
    </Modal>
  );
}
