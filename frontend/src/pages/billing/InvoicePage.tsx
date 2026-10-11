/** One invoice (`/billing/invoices/:invoiceId`, lake#564): its lines as issued, its state and how to
 * pay it. The account is the current workspace's, or `?customer_id=` for the operator, who can also
 * record a payment (`record_invoice_payment`) or void a due invoice (`void_invoice`). Reads
 * `get_invoice`. Only where the lake bills. */
import { useState } from "react";
import { useParams, useSearchParams } from "react-router-dom";
import { Ban, CircleCheck, Printer } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { Modal } from "@/lib/overlay";
import { dateTime } from "@/lib/format";
import { Banner, Button, Card, ErrorBlock, Field, Form, KeyValue, Page, Skeleton, Split, Status } from "@/lib/ui";
import { LinesTable } from "./BillingPage";
import { invoiceTone, monthLabel, money, type Invoice } from "./billing";

export default function InvoicePage() {
  const { invoiceId = "" } = useParams();
  const session = useSession();
  const [params] = useSearchParams();
  const customerId = params.get("customer_id") || session.space?.id || "";
  const operator = session.viewer.administrator;
  const [settling, setSettling] = useState<"paid" | "void" | null>(null);
  const invoice = useRead<Invoice>(customerId && invoiceId ? "get_invoice" : null, { customer_id: customerId, invoice_id: invoiceId }, { workspace: null });
  const title = invoice.data ? `Invoice ${invoice.data.number}` : "Invoice";
  const back = operator && customerId ? `/billing?customer_id=${encodeURIComponent(customerId)}` : "/billing";
  useTrail([{ label: "Billing", to: back }, entityCrumb(title)], title);

  return (
    <Page
      eyebrow={operator ? "Administration" : "Organization"}
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          {title}
          {invoice.data && <Status value={invoice.data.state} tone={invoiceTone(invoice.data.state)} />}
        </span>
      }
      sub={invoice.data ? `${monthLabel(invoice.data.period)} on the ${invoice.data.plan_name} plan.` : undefined}
      actions={
        <>
          <Button onClick={() => window.print()}>
            <Printer size={14} aria-hidden /> Print
          </Button>
          {operator && invoice.data?.state === "due" && (
            <>
              <Button onClick={() => setSettling("void")}>
                <Ban size={14} aria-hidden /> Void
              </Button>
              <Button variant="primary" onClick={() => setSettling("paid")}>
                <CircleCheck size={14} aria-hidden /> Record payment
              </Button>
            </>
          )}
        </>
      }
      wide
    >
      {invoice.isError ? (
        <ErrorBlock error={invoice.error} retry={() => void invoice.refetch()} what="this invoice" />
      ) : !invoice.data ? (
        <Skeleton lines={6} />
      ) : (
        <Split
          side={
            <>
              <Card label="Invoice">
                <KeyValue
                  rows={[
                    ["Number", <span className="font-mono">{invoice.data.number}</span>],
                    ["Month", monthLabel(invoice.data.period)],
                    ["Plan", invoice.data.plan_name],
                    ["Issued", dateTime(invoice.data.issued_at)],
                    ["State", <Status value={invoice.data.state} tone={invoiceTone(invoice.data.state)} />],
                    ...(invoice.data.paid_at ? ([["Paid", dateTime(invoice.data.paid_at)]] as [string, string][]) : []),
                    ...(invoice.data.voided_at ? ([["Voided", dateTime(invoice.data.voided_at)]] as [string, string][]) : []),
                    ...(invoice.data.provider_reference ? ([["Reference", <span className="font-mono">{invoice.data.provider_reference}</span>]] as [string, React.ReactNode][]) : []),
                  ]}
                />
              </Card>
              {invoice.data.state === "due" && invoice.data.instructions && (
                <Banner tone="info" title="How to Pay">
                  {invoice.data.instructions}
                </Banner>
              )}
              {invoice.data.note && (
                <Banner tone="quiet" title="Note">
                  {invoice.data.note}
                </Banner>
              )}
            </>
          }
        >
          <Card label="Lines" padded={false} footer={<span>Total <strong className="font-mono">{money(invoice.data.total_micros, invoice.data.currency)}</strong> ({invoice.data.currency})</span>}>
            <LinesTable lines={invoice.data.lines} currency={invoice.data.currency} />
          </Card>
        </Split>
      )}
      {settling && invoice.data && <Settle invoice={invoice.data} state={settling} onClose={() => setSettling(null)} />}
    </Page>
  );
}

function Settle({ invoice, state, onClose }: { invoice: Invoice; state: "paid" | "void"; onClose: () => void }) {
  const toast = useToast();
  const [reference, setReference] = useState("");
  const [note, setNote] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const paid = state === "paid";
  const submit = async () => {
    setError(null);
    setBusy(true);
    try {
      if (paid) await act("record_invoice_payment", { invoice_id: invoice.id, reference: reference || undefined, note: note || undefined });
      else await act("void_invoice", { invoice_id: invoice.id, note });
      toast(paid ? `Invoice ${invoice.number} paid` : `Invoice ${invoice.number} voided`);
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal
      title={paid ? "Record a Payment" : "Void the Invoice"}
      subtitle={paid ? `Marks invoice ${invoice.number} paid: ${money(invoice.total_micros, invoice.currency)}.` : `Invoice ${invoice.number} is cancelled, and ${monthLabel(invoice.period)} can be invoiced again.`}
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant={paid ? "primary" : "danger"} type="submit" form="settle-invoice" loading={busy}>
            {paid ? "Record payment" : "Void invoice"}
          </Button>
        </>
      }
    >
      <Form id="settle-invoice" onSubmit={submit} error={error}>
        {paid && (
          <Field label="Reference" help="The payment's reference, such as a bank transfer's.">
            <input value={reference} maxLength={256} onChange={(e) => setReference(e.target.value)} />
          </Field>
        )}
        <Field label={paid ? "Note" : "Why"} required={!paid}>
          <textarea value={note} maxLength={1024} rows={3} onChange={(e) => setNote(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}
