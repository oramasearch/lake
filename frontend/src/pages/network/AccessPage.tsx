/** Network → Developer Access (`/w/<id>/network/access`, lake#729, docs/ui/network.md): who may open
 * a tunnel from their own machine to one service or app port, until when. An account admin grants
 * (`grant_network_tunnel`) and revokes (`revoke_network_tunnel`); the person granted gets their own
 * certificate for a key that never leaves their machine (`issue_network_tunnel_certificate`). Recent
 * sessions come from `list_network_tunnel_sessions`. A tunnel is named for what it reaches. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { Eye, EyeOff, KeyRound, Plus } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { dateTime, relative } from "@/lib/format";
import { ConfirmButton, Drawer } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Actions, Button, CopyBlock, DataTable, Empty, Field, Form, Notice, Section, Status, Toolbar, fieldError } from "@/lib/ui";
import { useAsync } from "@/pages/train/workspace";
import {
  CSR_COMMAND,
  GRANT_DURATIONS,
  certificateFile,
  csrProblem,
  grantInput,
  grantReady,
  grantState,
  nameOf,
  recordColumns,
  str,
  tunnelInstructions,
  type GrantForm,
  type ServiceCredentials,
  type Names,
} from "./network";
import { Endpoint, NetworkPage, NotRecorded, RecordState, useNames, useNetworkRead, useScope } from "./parts";

export default function AccessPage() {
  // ?grant=<service id>&port=<port> (Ship → Services' Developer access) opens Grant access filled in.
  const [params, setParams] = useSearchParams();
  const prefill = params.get("grant") ? { targetKind: "service", targetId: params.get("grant") ?? "", port: params.get("port") ?? "" } : null;
  const [granting, setGranting] = useState(Boolean(prefill));
  const [issuing, setIssuing] = useState<Row | null>(null);
  const [history, setHistory] = useState(false);
  const { viewer } = useSession();
  const q = useNetworkRead<{ tunnel_grants?: Row[] }>("list_network_tunnel_grants");
  const sessions = useNetworkRead<Row>("list_network_tunnel_sessions", { limit: 50 });
  const { names, apps, services, people } = useNames();
  const all = q.data?.tunnel_grants ?? [];
  const now = Date.now();
  const rows = history ? all : all.filter((g) => grantState(g, now).label === "Active");
  const ended = all.length - all.filter((g) => grantState(g, now).label === "Active").length;
  const sessionRows = Array.isArray(sessions.data?.sessions) ? (sessions.data!.sessions as Row[]) : [];
  return (
    <NetworkPage
      title="Developer Access"
      sub="Who may open a tunnel from their own machine to one service or app in this workspace, and until when. Nothing is exposed to the internet: each person connects as themselves, with a certificate only they can use."
      actions={
        <Button variant="primary" onClick={() => setGranting(true)}>
          <Plus size={14} aria-hidden /> Grant access
        </Button>
      }
    >
      {ended > 0 && (
        <Toolbar>
          <label className="inline-flex items-center gap-2 text-[12.5px]">
            <input type="checkbox" checked={history} onChange={(e) => setHistory(e.target.checked)} /> Show {ended} expired or revoked
          </label>
        </Toolbar>
      )}
      <DataTable<Row>
        q={q}
        what="developer access"
        caption="Tunnel grants"
        rows={q.data ? rows : undefined}
        rowKey={(g) => String(g.id)}
        initialSort={{ key: "expires", dir: "ascending" }}
        empty={
          <Empty icon={KeyRound} title="No Developer Access" action={<Button variant="primary" onClick={() => setGranting(true)}>Grant access</Button>}>
            Grant a member of this account a tunnel to one port of a service or app, for as long as they need it.
          </Empty>
        }
        columns={[
          { key: "who", label: "Who", sort: (g) => names.user.get(String(g.user_id)) ?? String(g.user_id), render: (g) => <span className="font-semibold">{names.user.get(String(g.user_id)) ?? <span title={String(g.user_id)}>A member</span>}</span> },
          { key: "target", label: "Reaches", render: (g) => <Endpoint names={names} kind={String(g.target_kind)} id={String(g.target_id)} /> },
          { key: "port", label: "Port", numeric: true, sort: (g) => Number(g.port), render: (g) => <span className="font-mono">{String(g.port)}</span> },
          { key: "expires", label: "Until", sort: (g) => String(g.expires_at), render: (g) => <span className="font-mono text-[12px]" title={String(g.expires_at)}>{dateTime(g.expires_at)}</span> },
          {
            key: "state",
            label: "Access",
            render: (g) => {
              const state = grantState(g, now);
              return <Status value={state.label} tone={state.tone} />;
            },
          },
          { key: "relay", label: "Tunnel", render: (g) => (grantState(g, now).label === "Active" ? <RecordState record={g} /> : <span className="muted">—</span>) },
          {
            key: "actions",
            label: "",
            className: "text-right",
            render: (g) =>
              grantState(g, now).label === "Active" ? (
                <Actions className="justify-end">
                  {viewer.user_id && String(g.user_id) === viewer.user_id && (
                    <Button
                      variant="ghost"
                      disabled={!grantReady(g, now)}
                      title={grantReady(g, now) ? undefined : "The tunnel is not ready yet: the cluster has not reported its endpoint."}
                      onClick={() => setIssuing(g)}
                    >
                      Get credentials
                    </Button>
                  )}
                  <Revoke grant={g} names={names} />
                </Actions>
              ) : null,
          },
        ]}
      />
      <div className="mt-6" />
      <Section title="Recent Sessions" sub="Who connected through these grants, to what and when.">
        {sessions.data && sessions.data.available !== true ? (
          <NotRecorded what="Tunnel Sessions" page={sessions.data} />
        ) : (
          <DataTable<Row>
            q={sessions}
            what="tunnel sessions"
            rows={sessions.data ? sessionRows : undefined}
            empty="No tunnel sessions yet."
            columns={[
              { key: "when", label: "When", render: (r) => <span className="font-mono text-[12px]">{relative(recordColumns(r).when)}</span> },
              { key: "who", label: "Who", render: (r) => names.user.get(recordColumns(r).source) ?? (recordColumns(r).source || "—") },
              { key: "to", label: "Reached", render: (r) => recordColumns(r).destination || "—" },
              { key: "state", label: "State", render: (r) => recordColumns(r).outcome || "—" },
            ]}
          />
        )}
      </Section>
      {granting && (
        <Grant
          names={names}
          apps={apps}
          services={services}
          people={people}
          initial={prefill ?? undefined}
          you={viewer.user_id ? String(viewer.user_id) : undefined}
          onClose={() => {
            setGranting(false);
            if (prefill) setParams({}, { replace: true });
          }}
        />
      )}
      {issuing && <Issue grant={issuing} names={names} services={services} onClose={() => setIssuing(null)} />}
    </NetworkPage>
  );
}

function Revoke({ grant, names }: { grant: Row; names: Names }) {
  const scope = useScope();
  const toast = useToast();
  const who = names.user.get(String(grant.user_id)) ?? "This member";
  const target = nameOf(names, String(grant.target_kind), String(grant.target_id));
  return (
    <ConfirmButton
      variant="ghost"
      danger
      title="Revoke this access?"
      confirmLabel="Revoke"
      confirm={<>{who === "You" ? "You" : who} can no longer open a tunnel to {target}, and the cluster ends any session through it. Certificates already issued for it stop working.</>}
      onConfirm={async () => {
        await act("revoke_network_tunnel", { ...scope.input, tunnel_grant_id: String(grant.id) });
        toast(`Revoked access to ${target}`);
      }}
    >
      Revoke
    </ConfirmButton>
  );
}

/** "Grant access": a member, one port of a service or app, and how long. Only an account admin can. */
function Grant({
  names,
  apps,
  services,
  people,
  initial,
  you,
  onClose,
}: {
  names: Names;
  apps: Row[];
  services: Row[];
  people: { id: string; label: string }[];
  initial?: Partial<GrantForm>;
  you?: string;
  onClose: () => void;
}) {
  const scope = useScope();
  const toast = useToast();
  const save = useAsync();
  // From a service's Developer access, the grant is for the person asking, for an hour.
  const [form, setForm] = useState<GrantForm>({
    userId: initial ? (you ?? people[0]?.id ?? "") : (people[0]?.id ?? ""),
    targetKind: "service",
    targetId: "",
    port: "",
    hours: initial ? 1 : 8,
    ...initial,
  });
  const set = (patch: Partial<GrantForm>) => setForm((f) => ({ ...f, ...patch }));
  const targets = (form.targetKind === "app" ? apps : services).map((r) => ({ id: String(r.id), label: String(r.name || r.id) }));
  return (
    <Drawer
      title="Grant Developer Access"
      subtitle="One person, one port, for a set time. They connect from their own machine as themselves."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="network-grant" loading={save.busy}>
            Grant access
          </Button>
        </>
      }
    >
      <Form
        id="network-grant"
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            await act("grant_network_tunnel", { ...scope.input, ...grantInput(form) });
            toast(`Granted access to ${nameOf(names, form.targetKind, form.targetId)}`);
            onClose();
          })
        }
      >
        <Field label="Who" help="A member of this account." error={fieldError(save.error, "user_id")}>
          <select value={form.userId} onChange={(e) => set({ userId: e.target.value })}>
            {people.length === 0 && <option value="">No members found</option>}
            {people.map((p) => (
              <option key={p.id} value={p.id}>
                {p.label}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Reaches" error={fieldError(save.error, "target_id")}>
          <div className="flex gap-2">
            <select aria-label="Kind" value={form.targetKind} onChange={(e) => set({ targetKind: e.target.value, targetId: "" })} className="w-32">
              <option value="service">Service</option>
              <option value="app">App</option>
            </select>
            {targets.length === 0 ? (
              <span className="muted self-center text-[12.5px]">This workspace has no {form.targetKind === "app" ? "apps" : "services"} yet.</span>
            ) : (
              <select aria-label="Reaches" value={form.targetId} onChange={(e) => set({ targetId: e.target.value })} className="min-w-0 flex-1">
                <option value="">Choose…</option>
                {targets.map((t) => (
                  <option key={t.id} value={t.id}>
                    {t.label}
                  </option>
                ))}
              </select>
            )}
          </div>
        </Field>
        <Field label="Port" help="To reach a deployment, choose the service that serves it." error={fieldError(save.error, "port")}>
          <input inputMode="numeric" required placeholder="5432" value={form.port} onChange={(e) => set({ port: e.target.value })} />
        </Field>
        <Field label="For" error={fieldError(save.error, "expires_at")}>
          <select value={form.hours} onChange={(e) => set({ hours: Number(e.target.value) })}>
            {GRANT_DURATIONS.map((d) => (
              <option key={d.hours} value={d.hours}>
                {d.label}
              </option>
            ))}
          </select>
        </Field>
      </Form>
    </Drawer>
  );
}

/** "Get credentials": the recipient makes a key and a request on their machine, pastes the request,
 * and Lake signs it. The key never reaches Lake; the answer is shown once and not kept. */
function Issue({ grant, names, services, onClose }: { grant: Row; names: Names; services: Row[]; onClose: () => void }) {
  const scope = useScope();
  const sign = useAsync();
  const [csr, setCsr] = useState("");
  const [problem, setProblem] = useState<string | null>(null);
  const [result, setResult] = useState<Row | null>(null);
  // The service's own sign-in, read once the certificate is issued so step 5 is a working command.
  // `service_connection` answers an account admin; anyone else keeps the placeholders.
  const [credentials, setCredentials] = useState<ServiceCredentials | null>(null);
  const [revealed, setRevealed] = useState(false);
  const target = nameOf(names, String(grant.target_kind), String(grant.target_id));
  const serviceKind = services.find((s) => String(s.id) === String(grant.target_id))?.kind;
  const folder = `~/orama-tunnel/${String(grant.id).slice(0, 8)}`;
  const steps = result
    ? tunnelInstructions({
        grantId: String(grant.id),
        endpoint: String(result.endpoint ?? ""),
        port: Number(grant.port),
        targetKind: String(grant.target_kind),
        serviceKind: serviceKind ? String(serviceKind) : undefined,
        certificate: certificateFile(result),
        relayCa: String(result.relay_ca_pem ?? ""),
        credentials,
      })
    : null;
  return (
    <Drawer
      title="Get Tunnel Credentials"
      subtitle={`A certificate for your access to ${target} on port ${String(grant.port)}.`}
      onClose={onClose}
      width={640}
      footer={
        result ? (
          <Button variant="primary" onClick={onClose}>
            Done
          </Button>
        ) : (
          <>
            <Button onClick={onClose}>Cancel</Button>
            <Button variant="primary" type="submit" form="network-issue" loading={sign.busy}>
              Get certificate
            </Button>
          </>
        )
      }
    >
      {result ? (
        <div className="stack">
          <Notice tone="success" title="Certificate Issued">
            It is valid until {dateTime(result.expires_at)}. Lake does not keep a copy to show again; ask for another while your access lasts.
          </Notice>
          {steps && (
            <>
              <CopyBlock label="3. Save the certificate files (paste this in the same terminal)" value={steps.save} multiline />
              <CopyBlock label="4. Start the tunnel, and leave it running" value={steps.start} multiline />
              <p className="muted m-0 text-[12.5px]">
                This needs the tunnel client, <code>zetta-tunnel</code>. It listens only on your machine, at 127.0.0.1:{steps.localPort}, and stops working when your access ends or is revoked.
              </p>
              <CopyBlock
                label={`5. Connect to ${target}, in a second terminal`}
                value={steps.connect}
                shown={revealed ? steps.connect : steps.connectShown}
                extra={
                  steps.hasSecret ? (
                    <button type="button" onClick={() => setRevealed((r) => !r)} aria-pressed={revealed}>
                      {revealed ? <EyeOff size={14} aria-hidden /> : <Eye size={14} aria-hidden />} {revealed ? "Hide" : "Reveal"}
                    </button>
                  ) : undefined
                }
              />
              <p className="muted m-0 text-[12.5px]">{steps.connectNote}</p>
            </>
          )}
          <details>
            <summary className="muted text-[12.5px]">The files and the endpoint on their own</summary>
            <div className="stack mt-2">
              <CopyBlock label="Tunnel endpoint" value={String(result.endpoint ?? "")} />
              <CopyBlock label="tunnel.crt (certificate and chain)" value={certificateFile(result)} multiline />
              <CopyBlock label="relay-ca.crt (the tunnel's own certificate authority)" value={String(result.relay_ca_pem ?? "")} multiline />
            </div>
          </details>
        </div>
      ) : (
        <Form
          id="network-issue"
          error={problem ?? sign.error}
          onSubmit={() => {
            const found = csrProblem(csr);
            setProblem(found);
            if (found) return;
            return sign.run(async () => {
              setResult(await act<Row>("issue_network_tunnel_certificate", { ...scope.input, tunnel_grant_id: String(grant.id), csr_pem: csr.trim() + "\n" }));
              if (String(grant.target_kind) === "service" && serviceKind && serviceKind !== "zetta") {
                try {
                  // An action, so the password never rides a URL; as Show connection reads it.
                  const connection = await act<Row>("service_connection", { customer_id: scope.customerId, service_id: String(grant.target_id) });
                  setCredentials({ user: str(connection, "user"), password: str(connection, "password"), database: str(connection, "database") });
                } catch {
                  setCredentials(null);
                }
              }
            });
          }}
        >
          <CopyBlock label="1. In a terminal on your machine, make a key and a request" value={`mkdir -p ${folder} && cd ${folder}\n${CSR_COMMAND}\ncat tunnel.csr`} multiline />
          <Field label="2. Paste what it printed (tunnel.csr)" help="Only the request. tunnel.key stays on your machine, in that folder." error={fieldError(sign.error, "csr_pem")}>
            <textarea rows={8} className="font-mono" placeholder="-----BEGIN CERTIFICATE REQUEST-----" value={csr} onChange={(e) => setCsr(e.target.value)} spellCheck={false} />
          </Field>
        </Form>
      )}
    </Drawer>
  );
}
