/** Network → Policies (`/w/<id>/network/policies`, lake#729, docs/ui/network.md): the rules in
 * effect now (`network_rules`), first as sentences ("Reporting may reach orders on TCP 5432."), then
 * each one with whether the cluster has applied it. Read-only: the rules are changed on the pages
 * that own them. */
import { Link } from "react-router-dom";
import type { Row } from "@/lib/api";
import { dateTime } from "@/lib/format";
import { Card, DataTable, Query, Section } from "@/lib/ui";
import { useWsPath } from "@/pages/train/workspace";
import { ACCESS_PATH, CONNECTIONS_PATH, EGRESS_PATH, PEERS_PATH, egressDestination, nameOf, portsText, ruleSentences, type Names } from "./network";
import { NetworkPage, RecordState, useNames, useNetworkRead } from "./parts";

type Rule = { key: string; kind: string; path: string; what: string; record: Row };

export function flatten(rules: Row, names: Names): Rule[] {
  const list = (key: string) => (Array.isArray(rules[key]) ? (rules[key] as Row[]) : []);
  return [
    ...list("connections").map((c) => ({
      key: `c${String(c.id)}`,
      kind: "Connection",
      path: CONNECTIONS_PATH,
      what: `${nameOf(names, String(c.source_kind), String(c.source_id))} → ${nameOf(names, String(c.target_kind), String(c.target_id))} (${portsText(c.ports)})`,
      record: c,
    })),
    ...list("egress_rules").map((e) => ({
      key: `e${String(e.id)}`,
      kind: "Egress",
      path: EGRESS_PATH,
      what: `${e.source_kind ? nameOf(names, String(e.source_kind), String(e.source_id)) : "Workspace"} → ${egressDestination(e).value} (${portsText(e.ports)})`,
      record: e,
    })),
    ...list("tunnel_grants").map((g) => ({
      key: `t${String(g.id)}`,
      kind: "Developer access",
      path: ACCESS_PATH,
      what: `${names.user.get(String(g.user_id)) ?? "A member"} → ${nameOf(names, String(g.target_kind), String(g.target_id))}:${String(g.port)} until ${dateTime(g.expires_at)}`,
      record: g,
    })),
    ...list("peers").map((p) => ({ key: `p${String(p.id)}`, kind: "Private network", path: PEERS_PATH, what: String(p.name), record: p })),
  ];
}

export default function PoliciesPage() {
  const ws = useWsPath();
  const q = useNetworkRead<Row>("network_rules");
  const { names } = useNames();
  return (
    <NetworkPage title="Policies" sub="The rules in effect for this workspace, in words. Lake asks the cluster for exactly these paths and no others; each rule shows whether the cluster has applied it.">
      <Query q={q} what="the network rules">
        {(rules) => (
          <>
            <Card label="In effect" title="What This Workspace May Reach">
              <ul className="m-0 list-none space-y-1.5 p-0 text-[13.5px]">
                {ruleSentences(rules, names).map((sentence, i) => (
                  <li key={i}>{sentence}</li>
                ))}
              </ul>
            </Card>
            <div className="mt-6" />
            <Section title="Each Rule">
              <DataTable<Rule>
                what="rules"
                caption="Rules in effect"
                rows={flatten(rules, names)}
                rowKey={(r) => r.key}
                empty="No rules: nothing is asked for."
                columns={[
                  { key: "kind", label: "Kind", sort: (r) => r.kind, render: (r) => <Link to={ws(r.path)}>{r.kind}</Link> },
                  { key: "what", label: "Rule", render: (r) => r.what },
                  { key: "status", label: "Applied", render: (r) => <RecordState record={r.record} /> },
                ]}
              />
            </Section>
          </>
        )}
      </Query>
    </NetworkPage>
  );
}
