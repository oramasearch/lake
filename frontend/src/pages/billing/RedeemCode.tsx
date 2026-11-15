// SPDX-License-Identifier: MIT

/** Redeem a Code (lake#1156, docs/ui/balance.md#coupon-codes): the Billing page's card where an
 * account's owner or admin types a coupon code from an ad. `redeem_coupon` gives the account a
 * credit from Orama; the server checks the code, one redemption per account and per person, and
 * new-accounts-only. */
import { useState } from "react";
import { Gift } from "lucide-react";
import { act } from "@/lib/api";
import { normalizeCode } from "@/lib/coupon";
import { useToast } from "@/lib/toast";
import { Button, Card, Field, Form } from "@/lib/ui";

export function RedeemCode({ customerId, onRedeemed }: { customerId: string; onRedeemed?: () => void }) {
  const toast = useToast();
  const [code, setCode] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const normalized = normalizeCode(code);
  const submit = async () => {
    if (!normalized) return;
    setError(null);
    setBusy(true);
    try {
      const redeemed = await act<{ message: string }>("redeem_coupon", { customer_id: customerId, code: normalized }, { workspace: null });
      toast(redeemed.message);
      setCode("");
      onRedeemed?.();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Card label="Redeem a Code">
      <Form onSubmit={submit} error={error}>
        <Field label="Coupon code" help="A code from Orama adds credit to this account. Case does not matter.">
          <input name="code" value={code} onChange={(e) => setCode(e.target.value)} autoComplete="off" spellCheck={false} maxLength={64} placeholder="LAKE50" />
        </Field>
        <div>
          <Button type="submit" loading={busy} disabled={!normalized}>
            <Gift size={14} aria-hidden /> Redeem
          </Button>
        </div>
      </Form>
    </Card>
  );
}
