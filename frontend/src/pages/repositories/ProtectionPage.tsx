// SPDX-License-Identifier: MIT

/** /protection?repository_id= (web_ui_branch_protection): the branch protection rules and the
 * updates they refused, from the repository's workspace audit (or the central log when the
 * repository has no workspace). Rules are cards; refusals are a table. Anyone who can read the
 * repository sees its rules; the refusals come from the audit, which the account's administrators
 * and the operator may read (lake#777, lake#18), so a person who may not read it still gets the
 * rules and a sentence where the refusals would be, never a failed page. */
import { useSearchParams } from "react-router-dom";
import { Ban, ShieldCheck } from "lucide-react";
import { Card, DataTable, Empty, KeyValue, Section, Stat, StatRow } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import {
  ForgePage,
  ForgeStatus,
  Identifier,
  IdentifierList,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type AuditEvent = { id: string; occurred_at: string; kind: string; principal: string; details?: Record<string, any> };

export default function ProtectionPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Protection id={id} />;
}

function Protection({ id }: { id: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const rules = useForgeRead<Items<ForgeRecord>>("list_branch_protections", { repository_id: id, limit: 1000 });
  const workspaceId = repository.data?.workspace_id ?? "";
  const audit = useForgeRead<Items<AuditEvent>>(repository.data ? "audit" : null, workspaceId ? { limit: 500, workspace_id: workspaceId } : { limit: 500, log: "central" });
  useRepositoryTrail(repository.data, "Protection");
  const wait = pending(repository, rules);
  if (wait) return wait;
  const r = repository.data!;
  const ruleRows = itemsOf(rules.data);
  const auditReadable = !audit.isError;
  const refused = itemsOf(audit.data).filter((event) => event.kind === "forge.branch_protection.refused" && event.details?.repository === r.id);
  return (
    <ForgePage title="Branch Protection" repository={r} tab="protection" sub="The rules that guard references, and the updates they refused.">
      <StatRow cols={2}>
        <Stat label="Rules" value={ruleRows.length} icon={ShieldCheck} sub="protected references" />
        <Stat
          label="Refused updates"
          value={auditReadable ? refused.length : "—"}
          icon={Ban}
          tone={auditReadable && refused.length ? "bad" : undefined}
          sub={!auditReadable ? "the account's administrators can see these" : workspaceId ? "from the workspace audit" : "from the central log"}
        />
      </StatRow>
      <div className="space-y-6">
        <Section title="Rules">
          {ruleRows.length === 0 ? (
            <Empty compact icon={ShieldCheck} title="No branch protection rules" />
          ) : (
            <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
              {ruleRows.map((rule) => (
                <Card key={rule.id} label={<Identifier value={rule.id} />} title={<code>{rule.value.ref ?? ""}</code>}>
                  <KeyValue
                    rows={[
                      ["Review", <ForgeStatus value={rule.value.required_review ? "required" : "optional"} />],
                      ["Verification", <ForgeStatus value={rule.value.required_verification ? "required" : "optional"} />],
                      ["Updates", <ForgeStatus value={rule.value.fast_forward_only ? "fast-forward only" : "any"} />],
                      ["Allowed pushers", <IdentifierList values={rule.value.allowed_pushers} />],
                      ["Policy object", <Identifier value={rule.value.policy_object_id ?? ""} />],
                    ]}
                  />
                </Card>
              ))}
            </div>
          )}
        </Section>
        <Section title="Refused Updates">
          {!auditReadable ? (
            <Empty compact icon={Ban} title="Refused updates are in the audit">
              The account's administrators can see which updates these rules refused.
            </Empty>
          ) : (
          <DataTable<AuditEvent>
            what="refused updates"
            caption="Refused updates"
            rows={refused}
            rowKey={(event) => event.id}
            empty={<Empty compact icon={Ban} title="No protected updates have been refused" />}
            columns={[
              {
                key: "reference",
                label: "Reference",
                render: (event) => (
                  <div className="min-w-0">
                    <strong>{event.details?.reference ?? "reference"}</strong>
                    <div className="text-[12px] text-ink-3">{event.details?.reason ?? "update refused"}</div>
                  </div>
                ),
              },
              { key: "state", label: "Outcome", render: () => <ForgeStatus value="refused" /> },
              { key: "caller", label: "Caller", render: (event) => <Identifier value={event.details?.caller ?? event.principal} /> },
              { key: "rule", label: "Rule", render: (event) => event.details?.rule ?? "rule" },
              {
                key: "when",
                label: "When",
                sort: (event) => event.occurred_at,
                render: (event) => (
                  <span className="font-mono text-[12px] text-ink-2" title={dateTime(event.occurred_at)}>
                    {relative(event.occurred_at)}
                  </span>
                ),
              },
            ]}
          />
          )}
        </Section>
      </div>
    </ForgePage>
  );
}
