// SPDX-License-Identifier: MIT

/** Network → Egress and DNS (`/w/<id>/network/egress`, lake#729, docs/ui/network.md): the outside
 * destinations this workspace's workloads may reach, by DNS name or address range, for the whole
 * workspace or one workload (`list_network_egress_rules`), and "Allow a destination"
 * (`create_network_egress_rule`). The DNS names allowed are the rules written by name. */
import { useState } from "react";
import { Globe, Plus } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, Chip, DataTable, Empty, Field, Form, Mono, StateTabs, Toolbar, fieldError } from "@/lib/ui";
import { useAsync } from "@/pages/train/workspace";
import { egressDestination, egressInput, portsText, present, type EgressForm, type Names } from "./network";
import { Endpoint, NetworkPage, RecordState, useNames, useNetworkRead, useScope } from "./parts";

type View = "all" | "dns" | "cidr";

export default function EgressPage() {
  const [adding, setAdding] = useState(false);
  const [history, setHistory] = useState(false);
  const [view, setView] = useState<View>("all");
  const q = useNetworkRead<{ egress_rules?: Row[] }>("list_network_egress_rules");
  const { names, apps, services } = useNames();
  const all = q.data?.egress_rules ?? [];
  const live = present(all, false);
  const withdrawn = all.length - live.length;
  const rows = present(all, history).filter((r) => view === "all" || (view === "dns") === !!r.dns_name);
  const dnsNames = live.filter((r) => r.dns_name).length;
  return (
    <NetworkPage
      title="Egress and DNS"
      sub="The outside destinations this workspace's apps, services and agents may reach. A destination named by DNS is resolved and checked by name; nothing outside is reachable unless a rule here allows it."
      actions={
        <Button variant="primary" onClick={() => setAdding(true)}>
          <Plus size={14} aria-hidden /> Allow a destination
        </Button>
      }
    >
      {all.length > 0 && (
        <Toolbar>
          <StateTabs<View>
            value={view}
            onChange={setView}
            label="Destinations"
            tabs={[
              { value: "all", label: `All ${live.length}` },
              { value: "dns", label: `DNS names ${dnsNames}` },
              { value: "cidr", label: `Address ranges ${live.length - dnsNames}` },
            ]}
          />
          {withdrawn > 0 && (
            <label className="inline-flex items-center gap-2 text-[12.5px]">
              <input type="checkbox" checked={history} onChange={(e) => setHistory(e.target.checked)} /> Show {withdrawn} removed
            </label>
          )}
        </Toolbar>
      )}
      <DataTable<Row>
        q={q}
        what="egress rules"
        caption="Egress rules"
        rows={q.data ? rows : undefined}
        rowKey={(r) => String(r.id)}
        initialSort={{ key: "destination", dir: "ascending" }}
        empty={
          all.length === 0 ? (
            <Empty icon={Globe} title="No Outside Destinations" action={<Button variant="primary" onClick={() => setAdding(true)}>Allow a destination</Button>}>
              This workspace asks to reach nothing outside it. Allow a host such as api.example.com, or an address range, on the ports it needs.
            </Empty>
          ) : (
            <Empty compact title="No destination of this kind">
              Choose All to see every destination.
            </Empty>
          )
        }
        columns={[
          {
            key: "destination",
            label: "Destination",
            sort: (r) => egressDestination(r).value,
            render: (r) => {
              const d = egressDestination(r);
              return (
                <span className="inline-flex items-center gap-2">
                  <Mono>{d.value}</Mono>
                  <Chip>{d.kind}</Chip>
                </span>
              );
            },
          },
          { key: "ports", label: "Ports", render: (r) => <span className="font-mono text-[12px]">{portsText(r.ports)}</span> },
          { key: "for", label: "For", render: (r) => (r.source_kind ? <Endpoint names={names} kind={String(r.source_kind)} id={String(r.source_id)} /> : <span className="text-ink-2">The whole workspace</span>) },
          { key: "status", label: "Status", render: (r) => <RecordState record={r} /> },
          { key: "created", label: "Added", sort: (r) => String(r.created_at), render: (r) => <span className="font-mono text-[12px] text-ink-2">{relative(r.created_at)}</span> },
        ]}
      />
      {adding && <Allow names={names} apps={apps} services={services} onClose={() => setAdding(false)} />}
    </NetworkPage>
  );
}

function Allow({ apps, services, onClose }: { names: Names; apps: Row[]; services: Row[]; onClose: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const save = useAsync();
  const [form, setForm] = useState<EgressForm>({ appliesTo: "workspace", sourceId: "", destination: "dns", value: "", ports: "443" });
  const set = (patch: Partial<EgressForm>) => setForm((f) => ({ ...f, ...patch }));
  const sources = form.appliesTo === "app" ? apps : form.appliesTo === "service" ? services : [];
  return (
    <Drawer
      title="Allow a Destination"
      subtitle="One host or address range, on the ports it needs."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="network-egress" loading={save.busy}>
            Allow
          </Button>
        </>
      }
    >
      <Form
        id="network-egress"
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            const input = egressInput(form);
            await act("create_network_egress_rule", { ...scope.input, ...input });
            toast(`Allowed ${form.value.trim()}`);
            onClose();
          })
        }
      >
        <Field label="Destination" error={fieldError(save.error, "dns_name") ?? fieldError(save.error, "cidr")}>
          <div className="flex gap-2">
            <select aria-label="Destination kind" value={form.destination} onChange={(e) => set({ destination: e.target.value as EgressForm["destination"] })} className="w-40">
              <option value="dns">DNS name</option>
              <option value="cidr">Address range</option>
            </select>
            <input
              aria-label="Destination"
              className="min-w-0 flex-1 font-mono"
              placeholder={form.destination === "dns" ? "api.example.com" : "203.0.113.0/24"}
              value={form.value}
              onChange={(e) => set({ value: e.target.value })}
            />
          </div>
        </Field>
        <Field label="Ports" help="Separated by commas." error={fieldError(save.error, "ports")}>
          <input inputMode="numeric" required placeholder="443" value={form.ports} onChange={(e) => set({ ports: e.target.value })} />
        </Field>
        <Field label="For" error={fieldError(save.error, "source_id")}>
          <div className="flex gap-2">
            <select aria-label="Applies to" value={form.appliesTo} onChange={(e) => set({ appliesTo: e.target.value as EgressForm["appliesTo"], sourceId: "" })} className="w-44">
              <option value="workspace">The whole workspace</option>
              <option value="app">One app</option>
              <option value="service">One service</option>
            </select>
            {form.appliesTo !== "workspace" &&
              (sources.length === 0 ? (
                <span className="muted self-center text-[12.5px]">This workspace has no {form.appliesTo === "app" ? "apps" : "services"} yet.</span>
              ) : (
                <select aria-label="Workload" value={form.sourceId} onChange={(e) => set({ sourceId: e.target.value })} className="min-w-0 flex-1">
                  <option value="">Choose…</option>
                  {sources.map((s) => (
                    <option key={String(s.id)} value={String(s.id)}>
                      {String(s.name || s.id)}
                    </option>
                  ))}
                </select>
              ))}
          </div>
        </Field>
      </Form>
    </Drawer>
  );
}
