/** /protection?repository_id= (web_ui_branch_protection): the branch protection rules and the
 * updates they refused, from the repository's workspace audit (or the central log when the
 * repository has no workspace). */
import { useSearchParams } from "react-router-dom";
import { dateTime } from "@/lib/format";
import {
  ForgeBadge,
  ForgePage,
  ForgeTabs,
  Identifier,
  IdentifierList,
  itemsOf,
  pending,
  repositoryName,
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
  const wait = pending(repository, rules, audit);
  if (wait) return wait;
  const r = repository.data!;
  const ruleRows = itemsOf(rules.data);
  const refused = itemsOf(audit.data).filter((event) => event.kind === "forge.branch_protection.refused" && event.details?.repository === r.id);
  return (
    <ForgePage eyebrow="repository policy" title="Branch Protection" sub={repositoryName(r)}>
      <ForgeTabs repository={r} current="protection" />
      <section>
        <h2>Rules</h2>
        {ruleRows.length === 0 && <div className="panel empty">No branch protection rules.</div>}
        {ruleRows.map((rule) => (
          <article key={rule.id} className="panel protection-rule">
            <header>
              <h3>
                <code>{rule.value.ref ?? ""}</code>
              </h3>
              <Identifier value={rule.id} />
            </header>
            <dl>
              <dt>review</dt>
              <dd>
                <ForgeBadge value={rule.value.required_review ? "required" : "optional"} />
              </dd>
              <dt>verification</dt>
              <dd>
                <ForgeBadge value={rule.value.required_verification ? "required" : "optional"} />
              </dd>
              <dt>updates</dt>
              <dd>
                <ForgeBadge value={rule.value.fast_forward_only ? "fast-forward only" : "any"} />
              </dd>
              <dt>allowed pushers</dt>
              <dd>
                <IdentifierList values={rule.value.allowed_pushers} />
              </dd>
              <dt>policy object</dt>
              <dd>
                <Identifier value={rule.value.policy_object_id ?? ""} />
              </dd>
            </dl>
          </article>
        ))}
      </section>
      <section>
        <h2>Refused Updates</h2>
        {refused.map((event) => (
          <article key={event.id} className="panel protection-event">
            <header>
              <strong>{event.details?.reference ?? "reference"}</strong>
              <ForgeBadge value="refused" />
            </header>
            <p>{event.details?.reason ?? "update refused"}</p>
            <p className="sub">
              <Identifier value={event.details?.caller ?? event.principal} /> · {event.details?.rule ?? "rule"} · {dateTime(event.occurred_at)}
            </p>
          </article>
        ))}
        {refused.length === 0 && <div className="panel empty">No protected updates have been refused.</div>}
      </section>
    </ForgePage>
  );
}
