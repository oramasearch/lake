// SPDX-License-Identifier: MIT

/** Coupons (`/coupons`, lake#1156, docs/ui/balance.md#coupon-codes): the operator's coupon codes for
 * ads. Each code is worth an amount of credit from Orama; the page lists every code with its state
 * and its ads report (redemptions, first paid top-ups, spend), sums the same per campaign, creates a
 * code (typed or generated) and shows the link for the ad, and disables a code at once. Reads
 * `list_coupons`; writes `create_coupon` and `disable_coupon`. The operator only, where the lake
 * bills. Each region is a separate Lake: a code is created in each region the ad reaches. */
import { type ChangeEvent, useState } from "react";
import { Gift, Plus, Ticket } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { couponInput } from "@/lib/coupon";
import { date } from "@/lib/format";
import { ConfirmButton, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, Chip, CopyBlock, DataTable, Empty, Field, Form, Page, Stat, StatRow } from "@/lib/ui";
import { money } from "@/pages/billing/billing";

type Coupon = {
  id: string;
  code: string;
  amount_micros: number;
  credit_days: number;
  expires_at: string;
  max_redemptions: number | null;
  new_accounts_only: boolean;
  campaign: string;
  redemptions: number;
  state: "active" | "disabled" | "expired" | "used_up";
  created_at: string;
  redeemed: number;
  topped_up: number;
  spend_micros: number;
  paid_micros: number;
};
type Campaign = { campaign: string; coupons: number; redeemed: number; topped_up: number; spend_micros: number; paid_micros: number };
type Created = { coupon: Coupon; link: string; region: { name: string; label: string } | null; other_regions: { name: string; label: string; host: string }[] };

const STATE_TONE = { active: "ok", disabled: "neutral", expired: "warn", used_up: "warn" } as const;
const STATE_LABEL = { active: "Active", disabled: "Disabled", expired: "Expired", used_up: "Used up" } as const;

export default function CouponsPage() {
  const list = useRead<{ coupons: Coupon[]; campaigns: Campaign[] }>("list_coupons", undefined, { workspace: null });
  const [creating, setCreating] = useState(false);
  const [created, setCreated] = useState<Created | null>(null);
  const toast = useToast();
  const coupons = list.data?.coupons;
  const disable = async (coupon: Coupon) => {
    await act("disable_coupon", { coupon_id: coupon.id }, { workspace: null });
    toast(`${coupon.code} is disabled. Credit already given stays.`);
  };
  return (
    <Page
      eyebrow="Administration"
      title="Coupons"
      sub="Codes for ads. Each redemption gives the account a credit from Orama: spent before paid money, not cash, and it does not unlock the overdraft."
      actions={
        <Button variant="primary" onClick={() => setCreating(true)}>
          <Plus size={14} aria-hidden /> New code
        </Button>
      }
      wide
    >
      {created && (
        <Banner tone="ok" title={`Code ${created.coupon.code} Is Ready`} action={<Button variant="ghost" onClick={() => setCreated(null)}>Done</Button>}>
          <p className="m-0">Put this link in the ad. It works for {money(created.coupon.amount_micros, "USD")} of credit{created.region ? ` in ${created.region.label}` : ""}.</p>
          <CopyBlock value={created.link} label="Link for the ad" />
          {created.other_regions.length > 0 && (
            <p className="m-0 mt-2">
              Each region is a separate Lake. Create the same code, {created.coupon.code}, in {created.other_regions.map((r) => `${r.label} (${r.host})`).join(", ")} too, so the link works wherever a person chooses.
            </p>
          )}
        </Banner>
      )}
      {list.data && (
        <StatRow cols={3}>
          <Stat label="Codes" value={list.data.coupons.length} icon={Ticket} />
          <Stat label="Redeemed" value={list.data.coupons.reduce((sum, c) => sum + c.redeemed, 0)} icon={Gift} />
          <Stat label="Went On to Pay" value={list.data.coupons.reduce((sum, c) => sum + c.topped_up, 0)} sub="a first paid top-up after redeeming" />
        </StatRow>
      )}
      <Card label="Codes" padded={false}>
        <DataTable
          q={list}
          what="coupon codes"
          rows={coupons}
          rowKey={(c) => c.id}
          empty={
            <Empty icon={Ticket} title="No codes yet" action={<Button onClick={() => setCreating(true)}>New code</Button>}>
              Create a code for an ad. Whoever redeems it gets the credit you choose.
            </Empty>
          }
          columns={[
            { key: "code", label: "Code", sort: (c) => c.code, render: (c) => <span className="font-mono">{c.code}</span> },
            { key: "state", label: "State", sort: (c) => c.state, render: (c) => <Chip tone={STATE_TONE[c.state]}>{STATE_LABEL[c.state]}</Chip> },
            { key: "amount", label: "Credit", numeric: true, sort: (c) => c.amount_micros, render: (c) => <span title={`lasts ${c.credit_days} days after redemption`}>{money(c.amount_micros, "USD")}</span> },
            { key: "campaign", label: "Campaign", sort: (c) => c.campaign, render: (c) => c.campaign || "—" },
            { key: "who", label: "For", render: (c) => (c.new_accounts_only ? "New accounts" : "Any account") },
            { key: "redemptions", label: "Redemptions", numeric: true, sort: (c) => c.redemptions, render: (c) => (c.max_redemptions ? `${c.redemptions} of ${c.max_redemptions}` : String(c.redemptions)) },
            { key: "paid", label: "Went On to Pay", numeric: true, sort: (c) => c.topped_up, render: (c) => String(c.topped_up) },
            { key: "spend", label: "Spend", numeric: true, sort: (c) => c.spend_micros, render: (c) => money(c.spend_micros, "USD") },
            { key: "expires", label: "Code Expires", sort: (c) => c.expires_at, render: (c) => (c.expires_at ? date(c.expires_at) : "Never") },
            {
              key: "actions",
              label: "",
              render: (c) =>
                c.state === "disabled" ? null : (
                  <ConfirmButton variant="ghost" danger confirmLabel="Disable" title={`Disable ${c.code}?`} confirm="Later redemptions are refused at once. Credit already given stays." onConfirm={() => disable(c)}>
                    Disable
                  </ConfirmButton>
                ),
            },
          ]}
        />
      </Card>
      {list.data && list.data.campaigns.length > 0 && (
        <Card label="Campaigns" padded={false}>
          <DataTable
            rows={list.data.campaigns}
            rowKey={(c) => c.campaign || "(none)"}
            columns={[
              { key: "campaign", label: "Campaign", sort: (c) => c.campaign, render: (c) => c.campaign || "No campaign" },
              { key: "coupons", label: "Codes", numeric: true, render: (c) => String(c.coupons) },
              { key: "redeemed", label: "Redeemed", numeric: true, sort: (c) => c.redeemed, render: (c) => String(c.redeemed) },
              { key: "topped", label: "Went On to Pay", numeric: true, sort: (c) => c.topped_up, render: (c) => String(c.topped_up) },
              { key: "spend", label: "Spend", numeric: true, sort: (c) => c.spend_micros, render: (c) => money(c.spend_micros, "USD") },
              { key: "paid_in", label: "Paid In", numeric: true, sort: (c) => c.paid_micros, render: (c) => money(c.paid_micros, "USD") },
            ]}
          />
        </Card>
      )}
      {creating && (
        <NewCoupon
          onClose={() => setCreating(false)}
          onCreated={(answer) => {
            setCreated(answer);
            setCreating(false);
          }}
        />
      )}
    </Page>
  );
}

function NewCoupon({ onClose, onCreated }: { onClose: () => void; onCreated: (created: Created) => void }) {
  const [form, setForm] = useState({ code: "", dollars: "", days: "90", expires: "", max: "", newOnly: true, campaign: "" });
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const { input, problem } = couponInput(form);
  const set = (key: keyof typeof form) => (e: ChangeEvent<HTMLInputElement>) => setForm((f) => ({ ...f, [key]: key === "newOnly" ? e.target.checked : e.target.value }));
  const submit = async () => {
    if (!input) return;
    setError(null);
    setBusy(true);
    try {
      onCreated(await act<Created>("create_coupon", input, { workspace: null }));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title="New Code"
      subtitle="Whoever redeems the code gets a credit from Orama. One redemption per account and per person."
      onClose={onClose}
      width={560}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="new-coupon" loading={busy} disabled={!!problem}>
            Create code
          </Button>
        </>
      }
    >
      <Form id="new-coupon" onSubmit={submit} error={error}>
        <Field label="Code" help="Such as LAKE50. Leave it empty for a generated code no one can guess. Case does not matter.">
          <input name="code" value={form.code} onChange={set("code")} autoComplete="off" spellCheck={false} maxLength={64} />
        </Field>
        <Field label="Credit (USD)" required help="Up to $10,000.">
          <input name="amount" inputMode="decimal" value={form.dollars} onChange={set("dollars")} />
        </Field>
        <Field label="Credit lasts (days)" required help="From the redemption. What is left then stops counting.">
          <input name="days" inputMode="numeric" value={form.days} onChange={set("days")} />
        </Field>
        <Field label="Last day the code works (UTC)" help="Optional. Empty: until you disable it.">
          <input name="expires" type="date" value={form.expires} onChange={set("expires")} />
        </Field>
        <Field label="Most redemptions" help="Optional. Empty: no limit.">
          <input name="max" inputMode="numeric" value={form.max} onChange={set("max")} />
        </Field>
        <Field label="Campaign" help="For the ads report, such as google-2026-11.">
          <input name="campaign" value={form.campaign} onChange={set("campaign")} autoComplete="off" maxLength={64} />
        </Field>
        <label className="flex items-center gap-2 text-[13px]">
          <input type="checkbox" name="new-only" checked={form.newOnly} onChange={set("newOnly")} /> New accounts only (accounts made after the code, with no usage yet)
        </label>
        {problem && (form.code || form.dollars) ? <p className="m-0 text-[12.5px] text-ink-3">{problem}</p> : null}
      </Form>
    </Modal>
  );
}
