/** The price list (`/billing/plans`, lake#564): the operator's plans, each with its monthly fee and a
 * price per usage dimension (beyond what it includes), and a drawer that creates or replaces one
 * (`set_billing_plan`). Retired plans are listed too (`include_retired`). Only where the lake bills;
 * an account's own billing is `/billing?customer_id=`. */
import { useState } from "react";
import { Plus, Tags, Trash2 } from "lucide-react";
import { act, ApiError, useRead } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { useToast } from "@/lib/toast";
import { Drawer } from "@/lib/overlay";
import { relative } from "@/lib/format";
import { Badge, Button, Card, DataTable, Empty, Field, Form, Page, fieldError } from "@/lib/ui";
import { includedLabel, microsOf, money, priceInput, priceLabel, type Plan } from "./billing";

/** The dimensions a price can be set for, as usage_over_time names them (docs/api/metering.md). */
const DIMENSIONS: { name: string; title: string; unit: string }[] = [
  { name: "storage", title: "Storage", unit: "bytes" },
  { name: "transfer", title: "Transfer", unit: "bytes" },
  { name: "build_compute", title: "Build Compute", unit: "seconds" },
  { name: "run_compute", title: "Workspace Run Compute", unit: "seconds" },
  { name: "speech", title: "Speech to Text", unit: "seconds" },
  { name: "calls", title: "API and MCP Calls", unit: "calls" },
  { name: "predictions", title: "Model Predictions", unit: "calls" },
  { name: "model_tokens", title: "Model Tokens", unit: "tokens" },
  { name: "instances", title: "Service and App Instances", unit: "instance_seconds" },
  { name: "volumes", title: "Service and Store Volumes", unit: "gib_seconds" },
];

export default function PlansPage() {
  useTrail([{ label: "Billing" }, { label: "Price List" }], "Price List");
  const plans = useRead<{ plans: Plan[] }>("billing_plans", { include_retired: true }, { workspace: null });
  const [editing, setEditing] = useState<Plan | "new" | null>(null);
  return (
    <Page
      eyebrow="Administration"
      title="Price List"
      sub="The plans accounts choose from: a monthly fee, and a price for usage beyond what each plan includes."
      actions={
        <Button variant="primary" onClick={() => setEditing("new")}>
          <Plus size={14} aria-hidden /> New plan
        </Button>
      }
      wide
    >
      <Card padded={false}>
        <DataTable
          rows={plans.data?.plans}
          q={plans}
          what="the price list"
          rowKey={(p) => p.id}
          empty={
            <div className="p-4">
              <Empty icon={Tags} title="No plans yet" action={<Button variant="primary" onClick={() => setEditing("new")}>New plan</Button>}>
                Accounts can choose a plan once the price list has one.
              </Empty>
            </div>
          }
          columns={[
            {
              key: "name",
              label: "Plan",
              sort: (p) => p.position,
              render: (p) => (
                <div className="min-w-0">
                  <button type="button" className="cursor-pointer border-0 bg-transparent p-0 text-left font-semibold text-action hover:underline" onClick={() => setEditing(p)}>
                    {p.name}
                  </button>
                  <div className="text-[12px] text-ink-3">
                    <span className="font-mono">{p.id}</span>
                    {p.description ? ` · ${p.description}` : ""}
                  </div>
                </div>
              ),
            },
            { key: "fee", label: "Monthly Fee", numeric: true, sort: (p) => p.base_micros, render: (p) => <span className="font-mono">{money(p.base_micros, p.currency)}</span> },
            {
              key: "prices",
              label: "Usage Prices",
              render: (p) =>
                p.prices.length ? (
                  <ul className="m-0 list-none p-0 text-[12.5px]">
                    {p.prices.map((price) => (
                      <li key={price.dimension}>
                        {price.title}: {priceLabel(price, p.currency)}
                        {includedLabel(price) ? `, ${includedLabel(price)}` : ""}
                      </li>
                    ))}
                  </ul>
                ) : (
                  <span className="text-ink-3">None</span>
                ),
            },
            { key: "state", label: "State", render: (p) => (p.active ? <Badge tone="success">Offered</Badge> : <Badge>Retired</Badge>) },
            { key: "updated", label: "Updated", sort: (p) => p.updated_at ?? "", render: (p) => <span className="text-ink-3">{relative(p.updated_at)}</span> },
          ]}
        />
      </Card>
      {editing && <EditPlan plan={editing === "new" ? null : editing} onClose={() => setEditing(null)} />}
    </Page>
  );
}

type PriceRow = { dimension: string; price: string; per: string; included: string };

function EditPlan({ plan, onClose }: { plan: Plan | null; onClose: () => void }) {
  const toast = useToast();
  const [id, setId] = useState(plan?.id ?? "");
  const [name, setName] = useState(plan?.name ?? "");
  const [description, setDescription] = useState(plan?.description ?? "");
  const [currency, setCurrency] = useState(plan?.currency ?? "USD");
  const [fee, setFee] = useState(plan ? String(plan.base_micros / 1_000_000) : "0");
  const [position, setPosition] = useState(String(plan?.position ?? 0));
  const [active, setActive] = useState(plan?.active ?? true);
  const [rows, setRows] = useState<PriceRow[]>(
    (plan?.prices ?? []).map((p) => ({ dimension: p.dimension, price: String(p.unit_micros / 1_000_000), per: String(p.per), included: String(p.included) })),
  );
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const update = (index: number, change: Partial<PriceRow>) => setRows(rows.map((r, i) => (i === index ? { ...r, ...change } : r)));
  const unused = DIMENSIONS.filter((d) => !rows.some((r) => r.dimension === d.name));
  const submit = async () => {
    setError(null);
    const base = microsOf(fee);
    if (base === null) {
      setError(new ApiError(400, "invalid_input", "the monthly fee is an amount, such as 20 or 19.99", "/base_micros"));
      return;
    }
    const prices = rows.map(priceInput);
    const bad = prices.findIndex((p) => p === null);
    if (bad >= 0) {
      setError(new ApiError(400, "invalid_input", `price ${bad + 1}: a price is an amount, "per" a whole number above 0 and "included" a whole number`, `/prices/${bad}`));
      return;
    }
    setBusy(true);
    try {
      await act("set_billing_plan", { id, name, description, currency, base_micros: base, position: Number(position) || 0, active, prices });
      toast(plan ? `${name} saved` : `${name} added to the price list`);
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const unitOf = (dimension: string) => DIMENSIONS.find((d) => d.name === dimension)?.unit ?? "";
  return (
    <Drawer
      title={plan ? `Edit ${plan.name}` : "New Plan"}
      subtitle="Each price buys the given number of units (bytes, seconds, calls, tokens), charged beyond what the plan includes each month."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="billing-plan" loading={busy}>
            Save plan
          </Button>
        </>
      }
    >
      <Form id="billing-plan" onSubmit={submit} error={fieldError(error, "id") || fieldError(error, "base_micros") ? null : error}>
        <Field label="Id" required help="Lower-case letters, digits and hyphens; it never changes." error={fieldError(error, "id")}>
          <input value={id} onChange={(e) => setId(e.target.value)} disabled={!!plan} pattern="[a-z][a-z0-9-]{0,62}" />
        </Field>
        <Field label="Name" required>
          <input value={name} maxLength={128} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Description">
          <textarea value={description} maxLength={1024} rows={2} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
          <Field label="Monthly fee" required error={fieldError(error, "base_micros")}>
            <input inputMode="decimal" value={fee} onChange={(e) => setFee(e.target.value)} />
          </Field>
          <Field label="Currency" required>
            <input value={currency} maxLength={3} onChange={(e) => setCurrency(e.target.value.toUpperCase())} />
          </Field>
          <Field label="Order" help="Lower comes first.">
            <input inputMode="numeric" value={position} onChange={(e) => setPosition(e.target.value)} />
          </Field>
        </div>
        <label className="flex items-center gap-2 text-[13px]">
          <input type="checkbox" checked={active} onChange={(e) => setActive(e.target.checked)} /> Offered to accounts (unchecked, the plan is retired: accounts on it stay on it)
        </label>
        <div className="label mt-2">Usage Prices</div>
        {rows.map((row, index) => (
          <div key={index} className="grid grid-cols-2 items-end gap-2 sm:grid-cols-[2fr_1fr_1fr_1fr_auto]">
            <Field label="Dimension">
              <select value={row.dimension} onChange={(e) => update(index, { dimension: e.target.value })}>
                {DIMENSIONS.filter((d) => d.name === row.dimension || !rows.some((r) => r.dimension === d.name)).map((d) => (
                  <option key={d.name} value={d.name}>
                    {d.title}
                  </option>
                ))}
              </select>
            </Field>
            <Field label={`Price (${currency})`}>
              <input inputMode="decimal" value={row.price} onChange={(e) => update(index, { price: e.target.value })} />
            </Field>
            <Field label={`Per (${unitOf(row.dimension)})`}>
              <input inputMode="numeric" value={row.per} onChange={(e) => update(index, { per: e.target.value })} />
            </Field>
            <Field label="Included">
              <input inputMode="numeric" value={row.included} onChange={(e) => update(index, { included: e.target.value })} />
            </Field>
            <Button variant="ghost" onClick={() => setRows(rows.filter((_, i) => i !== index))} title="Remove this price">
              <Trash2 size={14} aria-hidden />
            </Button>
          </div>
        ))}
        {unused.length > 0 && (
          <Button onClick={() => setRows([...rows, { dimension: unused[0].name, price: "0", per: "1", included: "0" }])}>
            <Plus size={14} aria-hidden /> Add a price
          </Button>
        )}
      </Form>
    </Drawer>
  );
}
