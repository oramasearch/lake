/** Billing (`/billing`, lake#564): an account's plan, this month's bill so far, plan selection and the
 * invoice history. The account is the current workspace's (`session.space`); the operator opens one
 * with `?customer_id=` and can issue an invoice for a month that has ended. Reads `billing_account`,
 * `billing_plans`, `bill_preview` and `list_invoices`; writes `choose_billing_plan` and, for the
 * operator, `issue_invoice`. The page exists only where the lake bills (`viewer.billing`). */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Check, FileText, Receipt, Tags } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { ConfirmButton, Modal } from "@/lib/overlay";
import { date } from "@/lib/format";
import { Badge, Banner, Button, Card, DataTable, Empty, ErrorBlock, Field, Form, Grid, Page, Skeleton, Split, Stat, StatRow, Status } from "@/lib/ui";
import {
  choiceOf,
  choiceSentence,
  includedLabel,
  invoiceTone,
  monthLabel,
  months,
  money,
  owed,
  paymentSummary,
  priceLabel,
  quantityLabel,
  type Account,
  type Bill,
  type Invoice,
  type Line,
  type Plan,
} from "./billing";

export default function BillingPage() {
  const session = useSession();
  const { viewer } = session;
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") || session.space?.id || "";
  const operator = viewer.administrator;
  const [issuing, setIssuing] = useState(false);
  useTrail([{ label: "Billing" }], "Billing");

  const account = useRead<Account>(customerId ? "billing_account" : null, { customer_id: customerId }, { workspace: null });
  const plans = useRead<{ plans: Plan[] }>("billing_plans", undefined, { workspace: null });
  const invoices = useRead<{ invoices: Invoice[] }>(customerId ? "list_invoices" : null, { customer_id: customerId }, { workspace: null });
  const hasPlan = !!account.data?.current;
  const bill = useRead<Bill>(customerId && hasPlan ? "bill_preview" : null, { customer_id: customerId }, { workspace: null, staleTime: 60_000 });

  if (!customerId) {
    return (
      <Page eyebrow={operator ? "Administration" : "Organization"} title="Billing">
        <Empty icon={Receipt} title={operator ? "Choose an account" : "Start your first workspace"} action={operator ? <Button to="/customers">Customers</Button> : <Button to="/workspaces" variant="primary">Workspaces</Button>}>
          {operator ? "Open an account's billing from its customer page, or add ?customer_id= to this address." : "Billing belongs to the account that owns your workspaces."}
        </Empty>
      </Page>
    );
  }

  const due = owed(invoices.data?.invoices ?? []);
  const current = account.data?.current?.plan ?? null;
  const next = account.data?.next ?? null;
  const nextMonth = next?.effective_from.slice(0, 7) ?? nextMonthOf(account.data?.month);
  return (
    <Page
      eyebrow={operator ? "Administration" : "Organization"}
      title="Billing"
      sub="Your plan, this month's bill so far, and your invoices. The bill is worked out from your usage."
      actions={
        <>
          <Button to="/usage">Usage</Button>
          {operator && (
            <Button variant="primary" onClick={() => setIssuing(true)}>
              <FileText size={14} aria-hidden /> Issue an invoice
            </Button>
          )}
        </>
      }
      wide
    >
      {account.isError ? (
        <ErrorBlock error={account.error} retry={() => void account.refetch()} what="this account's billing" />
      ) : !account.data ? (
        <Skeleton lines={6} />
      ) : (
        <>
          <StatRow cols={4}>
            <Stat label="Plan" value={current?.name ?? "None"} icon={Tags} sub={next ? `${next.plan?.name ?? next.plan_id} from ${monthLabel(nextMonth)}` : current ? `since ${monthLabel(account.data.current!.effective_from)}` : "choose a plan below"} />
            <Stat label="This Month So Far" value={bill.data ? money(bill.data.total_micros, bill.data.currency) : "—"} sub={bill.data ? monthLabel(bill.data.period) : hasPlan ? "reading usage" : "no plan yet"} />
            <Stat label="Due" value={due.length ? due.map((d) => money(d.micros, d.currency)).join(" + ") : "Nothing"} tone={due.length ? "bad" : undefined} sub={`${(invoices.data?.invoices ?? []).filter((i) => i.state === "due").length} invoices due`} />
            <Stat label="Payment" value={paymentSummary(account.data.provider).value} sub={paymentSummary(account.data.provider).sub} />
          </StatRow>
          {account.data.provider.reason && (
            <Banner tone="quiet" title="Invoice Only">
              {operator ? account.data.provider.reason : "Card payment is not set up on this lake yet. Orama sends payment details with each invoice and records your payment."}
            </Banner>
          )}
          {!hasPlan && (
            <Banner tone="info" title="Choose a Plan">
              Your account has no plan yet. A plan you choose now applies from the start of this month.
            </Banner>
          )}
          <Split
            side={
              <>
                <InvoicesCard invoices={invoices} customerId={customerId} operator={operator} />
                {bill.data && bill.data.not_measured.length > 0 && (
                  <Banner tone="quiet" title="Not Measured Yet">
                    <ul className="m-0 pl-4">
                      {bill.data.not_measured.map((line, i) => (
                        <li key={i}>{line}</li>
                      ))}
                    </ul>
                  </Banner>
                )}
              </>
            }
          >
            {hasPlan && <BillCard bill={bill} />}
            <PlansCard plans={plans} account={account.data} customerId={customerId} nextMonth={nextMonth} />
          </Split>
        </>
      )}
      {issuing && <IssueInvoice customerId={customerId} onClose={() => setIssuing(false)} />}
    </Page>
  );
}

function nextMonthOf(month: string | undefined): string {
  const match = /^(\d{4})-(\d{2})$/.exec(month ?? "");
  if (!match) return months().current;
  const year = Number(match[1]);
  const m = Number(match[2]);
  return m === 12 ? `${year + 1}-01` : `${year}-${String(m + 1).padStart(2, "0")}`;
}

/** The lines of a bill or an invoice. */
export function LinesTable({ lines, currency }: { lines: Line[]; currency: string }) {
  return (
    <DataTable
      rows={lines}
      rowKey={(l, i) => `${l.kind}-${l.dimension}-${i}`}
      columns={[
        {
          key: "description",
          label: "Item",
          render: (l) => (
            <div className="min-w-0">
              <div className="font-semibold text-ink">{l.description}</div>
              {l.kind === "usage" && <div className="text-[12px] text-ink-3">{l.unit_micros ? priceLabel({ unit_micros: l.unit_micros, per: l.per, unit: l.unit }, currency) : "not charged"}{includedLabel(l) ? ` · ${includedLabel(l)}` : ""}</div>}
            </div>
          ),
        },
        { key: "quantity", label: "Used", numeric: true, render: (l) => <span className="font-mono">{quantityLabel(l)}</span> },
        { key: "billable", label: "Billed", numeric: true, render: (l) => <span className="font-mono">{l.kind === "plan" ? "—" : quantityLabel({ ...l, quantity: l.billable })}</span> },
        { key: "amount", label: "Amount", numeric: true, render: (l) => <span className="font-mono">{money(l.amount_micros, currency)}</span> },
      ]}
    />
  );
}

function BillCard({ bill }: { bill: ReturnType<typeof useRead<Bill>> }) {
  return (
    <Card label="This Month So Far" title={bill.data ? `${monthLabel(bill.data.period)} on ${bill.data.plan.name}` : undefined} padded={false} footer={bill.data && <span>Total so far <strong className="font-mono">{money(bill.data.total_micros, bill.data.currency)}</strong>. The invoice is issued after the month ends.</span>}>
      {bill.isError ? (
        <div className="p-4">
          <ErrorBlock error={bill.error} retry={() => void bill.refetch()} what="this month's bill" />
        </div>
      ) : !bill.data ? (
        <div className="p-4">
          <Skeleton lines={4} />
        </div>
      ) : (
        <>
          {!bill.data.complete && (
            <div className="px-4 pt-3">
              <Banner tone="warn" title="Some Usage Could Not Be Read">
                {(bill.data.unread_workspaces ?? []).map((w) => w.name).join(", ")}: these workspaces did not answer, so this bill is short until they do.
              </Banner>
            </div>
          )}
          <LinesTable lines={bill.data.lines} currency={bill.data.currency} />
        </>
      )}
    </Card>
  );
}

function PlansCard({ plans, account, customerId, nextMonth }: { plans: ReturnType<typeof useRead<{ plans: Plan[] }>>; account: Account; customerId: string; nextMonth: string }) {
  const toast = useToast();
  return (
    <Card label="Plans" title="Choose a Plan" className="mt-4">
      {plans.isError ? (
        <ErrorBlock error={plans.error} retry={() => void plans.refetch()} what="the plans" />
      ) : !plans.data ? (
        <Skeleton lines={4} />
      ) : plans.data.plans.length === 0 ? (
        <Empty icon={Tags} title="No plans yet">
          Orama has not published a price list on this lake.
        </Empty>
      ) : (
        <Grid className="grid-cols-1 gap-3 md:grid-cols-2">
          {plans.data.plans.map((plan) => {
            const choice = choiceOf(account, plan.id);
            const isCurrent = account.current?.plan_id === plan.id;
            const isNext = account.next?.plan_id === plan.id;
            return (
              <Card key={plan.id} tone={isCurrent ? "active" : "default"} title={plan.name} label={isCurrent ? "Current Plan" : isNext ? `From ${monthLabel(nextMonth)}` : undefined} actions={isCurrent && !account.next ? <Badge tone="success">Current</Badge> : undefined}>
                <div className="font-mono text-[20px] font-semibold text-ink">
                  {money(plan.base_micros, plan.currency)}
                  <span className="text-[12px] font-normal text-ink-3"> a month</span>
                </div>
                {plan.description && <p className="m-0 mt-1 text-[13px] text-ink-2">{plan.description}</p>}
                <ul className="m-0 mt-2 list-none p-0 text-[12.5px] text-ink-2">
                  {plan.prices.map((p) => (
                    <li key={p.dimension} className="flex items-start gap-1.5 py-0.5">
                      <Check size={13} className="mt-0.5 shrink-0 text-ok" aria-hidden />
                      <span>
                        <strong className="font-semibold text-ink">{p.title}</strong>: {includedLabel(p) ? `${includedLabel(p)}, then ` : ""}
                        {p.unit_micros ? priceLabel(p, plan.currency) : "not charged"}
                      </span>
                    </li>
                  ))}
                  {plan.prices.length === 0 && <li className="py-0.5">Usage is not charged on this plan.</li>}
                </ul>
                {choice !== "current" && !isNext && (
                  <div className="mt-3">
                    <ConfirmButton
                      variant={choice === "start" ? "primary" : "secondary"}
                      title={choice === "cancel" ? `Stay on ${plan.name}` : `Choose ${plan.name}`}
                      confirm={choiceSentence(choice, plan.name, nextMonth)}
                      confirmLabel={choice === "cancel" ? "Stay" : "Choose plan"}
                      onConfirm={async () => {
                        await act("choose_billing_plan", { customer_id: customerId, plan_id: plan.id });
                        toast(choice === "start" ? `${plan.name} plan started` : choice === "cancel" ? "Plan change cancelled" : `${plan.name} from ${monthLabel(nextMonth)}`);
                      }}
                    >
                      {choice === "cancel" ? `Stay on ${plan.name}` : `Choose ${plan.name}`}
                    </ConfirmButton>
                  </div>
                )}
              </Card>
            );
          })}
        </Grid>
      )}
    </Card>
  );
}

function InvoicesCard({ invoices, customerId, operator }: { invoices: ReturnType<typeof useRead<{ invoices: Invoice[] }>>; customerId: string; operator: boolean }) {
  const suffix = operator ? `?customer_id=${encodeURIComponent(customerId)}` : "";
  return (
    <Card label="Invoices" padded={false}>
      <DataTable
        rows={invoices.data?.invoices}
        q={invoices}
        what="the invoices"
        rowKey={(i) => i.id}
        empty={
          <div className="p-4">
            <Empty icon={Receipt} title="No invoices yet">
              An invoice is issued for each month after it ends.
            </Empty>
          </div>
        }
        columns={[
          {
            key: "period",
            label: "Month",
            sort: (i) => i.period,
            render: (i) => (
              <div className="min-w-0">
                <Link to={`/billing/invoices/${encodeURIComponent(i.id)}${suffix}`} className="font-semibold">
                  {monthLabel(i.period)}
                </Link>
                <div className="text-[12px] text-ink-3">
                  No. {i.number} · {date(i.issued_at)}
                </div>
              </div>
            ),
          },
          { key: "total", label: "Total", numeric: true, sort: (i) => i.total_micros, render: (i) => <span className="font-mono">{money(i.total_micros, i.currency)}</span> },
          { key: "state", label: "State", render: (i) => <Status value={i.state} tone={invoiceTone(i.state)} /> },
        ]}
      />
    </Card>
  );
}

function IssueInvoice({ customerId, onClose }: { customerId: string; onClose: () => void }) {
  const toast = useToast();
  const [period, setPeriod] = useState(months().previous);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setError(null);
    setBusy(true);
    try {
      const invoice = await act<Invoice>("issue_invoice", { customer_id: customerId, period });
      toast(`Invoice ${invoice.number} issued: ${money(invoice.total_micros, invoice.currency)}`);
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title="Issue an Invoice"
      subtitle="The bill for a month that has ended, from every workspace's usage on the plan of that month. Its lines are kept as issued."
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="issue-invoice" loading={busy}>
            Issue invoice
          </Button>
        </>
      }
    >
      <Form id="issue-invoice" onSubmit={submit} error={error}>
        <Field label="Month" required help="A UTC calendar month that has ended.">
          <input type="month" value={period} onChange={(e) => setPeriod(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}
