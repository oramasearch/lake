/** Billing (`/billing`, lake#564, lake#635): this month's usage per dimension × its rate = cost, the
 * running total, the invoices and how they are paid. Lake is priced by usage only: every account is
 * billed at the one price list, with no plan to choose, no monthly fee and nothing included. The
 * account is the current workspace's (`session.space`); the operator opens one with `?customer_id=`
 * and can issue an invoice for a month that has ended. Reads `billing_account`, `price_list`,
 * `bill_preview` and `list_invoices`; writes only `issue_invoice`, for the operator. The page exists
 * only where the lake bills (`viewer.billing`). */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { FileText, Receipt, Tags } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { Modal } from "@/lib/overlay";
import { date } from "@/lib/format";
import { Banner, Button, Card, DataTable, Empty, ErrorBlock, Field, Form, Page, Skeleton, Split, Stat, StatRow, Status } from "@/lib/ui";
import { billedNote, invoiceTone, lineSentence, monthLabel, months, money, owed, paymentSummary, priceLabel, quantityLabel, type Account, type Bill, type Invoice, type Line, type PriceListAnswer } from "./billing";

export default function BillingPage() {
  const session = useSession();
  const { viewer } = session;
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") || session.space?.id || "";
  const operator = viewer.administrator;
  const [issuing, setIssuing] = useState(false);
  useTrail([{ label: "Billing" }], "Billing");

  const account = useRead<Account>(customerId ? "billing_account" : null, { customer_id: customerId }, { workspace: null });
  const prices = useRead<PriceListAnswer>("price_list", undefined, { workspace: null });
  const invoices = useRead<{ invoices: Invoice[] }>(customerId ? "list_invoices" : null, { customer_id: customerId }, { workspace: null });
  const hasList = !!account.data?.price_list;
  const bill = useRead<Bill>(customerId && hasList ? "bill_preview" : null, { customer_id: customerId }, { workspace: null, staleTime: 60_000 });

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
  const next = prices.data?.next ?? null;
  return (
    <Page
      eyebrow={operator ? "Administration" : "Organization"}
      title="Billing"
      sub="You pay for what you use: each kind of usage at its rate, with no plan and no monthly fee."
      actions={
        <>
          <Button to="/usage">Usage</Button>
          {operator && <Button to="/billing/plans">Price list</Button>}
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
          <StatRow cols={3}>
            <Stat label="This Month So Far" value={bill.data ? money(bill.data.total_micros, bill.data.currency) : "—"} sub={bill.data ? monthLabel(bill.data.period) : hasList ? "reading usage" : "no price list yet"} />
            <Stat label="Due" value={due.length ? due.map((d) => money(d.micros, d.currency)).join(" + ") : "Nothing"} tone={due.length ? "bad" : undefined} sub={`${(invoices.data?.invoices ?? []).filter((i) => i.state === "due").length} invoices due`} />
            <Stat label="Payment" value={paymentSummary(account.data.provider).value} sub={paymentSummary(account.data.provider).sub} />
          </StatRow>
          {account.data.provider.reason && (
            <Banner tone="quiet" title="Invoice Only">
              {operator ? account.data.provider.reason : "Card payment is not set up on this lake yet. Orama sends payment details with each invoice and records your payment."}
            </Banner>
          )}
          {!hasList && (
            <Banner tone="info" title="No Price List Yet" action={operator ? <Button to="/billing/plans">Publish it</Button> : undefined}>
              {operator ? "Publish the price list so accounts can be billed for their usage." : "Orama has not published its rates on this lake yet. Your usage is recorded, and nothing is billed until it is."}
            </Banner>
          )}
          {next && (
            <Banner tone="info" title={`New Rates From ${monthLabel(next.effective_from)}`}>
              The price list changes from {monthLabel(next.effective_from)}. This month is billed at today's rates.
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
            {hasList ? (
              <BillCard bill={bill} />
            ) : (
              <Card label="This Month So Far">
                <Empty icon={Tags} title="Nothing to bill yet">
                  The bill appears here once a price list is published.
                </Empty>
              </Card>
            )}
          </Split>
        </>
      )}
      {issuing && <IssueInvoice customerId={customerId} onClose={() => setIssuing(false)} />}
    </Page>
  );
}

/** The lines of a bill or an invoice: each dimension's usage, its rate and its cost. */
export function LinesTable({ lines, currency }: { lines: Line[]; currency: string }) {
  return (
    <DataTable
      rows={lines}
      rowKey={(l, i) => `${l.kind}-${l.dimension}-${i}`}
      columns={[
        { key: "description", label: "Usage", render: (l) => <span className="font-semibold text-ink">{l.description}</span> },
        {
          key: "quantity",
          label: "Used",
          numeric: true,
          render: (l) => (
            <div>
              <span className="font-mono">{quantityLabel(l)}</span>
              {billedNote(l) && <div className="text-[12px] text-ink-3">{billedNote(l)}</div>}
            </div>
          ),
        },
        { key: "rate", label: "Rate", numeric: true, render: (l) => <span className="text-ink-2">{l.unit_micros ? priceLabel(l, currency) : "not charged"}</span> },
        { key: "amount", label: "Cost", numeric: true, render: (l) => <span className="font-mono" title={lineSentence(l, currency)}>{money(l.amount_micros, currency)}</span> },
      ]}
    />
  );
}

function BillCard({ bill }: { bill: ReturnType<typeof useRead<Bill>> }) {
  return (
    <Card label="This Month So Far" title={bill.data ? monthLabel(bill.data.period) : undefined} padded={false} footer={bill.data && <span>Total so far <strong className="font-mono">{money(bill.data.total_micros, bill.data.currency)}</strong>. The month is billed at the rates in force at its end, and invoiced after it ends.</span>}>
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
      subtitle="The bill for a month that has ended: every workspace's usage at the rates in force at the month's end. Its lines are kept as issued."
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
