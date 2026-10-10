/** /issues?repository_id=&state=&label=&assignee= (web_ui_issues): the issue tracker, filtered in
 * the page by state, label and assignee as the renderer did (`list_issues` takes no filters). */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { date } from "@/lib/format";
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

export default function IssuesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Issues id={id} />;
}

function Issues({ id }: { id: string }) {
  const [params, setParams] = useSearchParams();
  const state = params.get("state") ?? "";
  const label = params.get("label") ?? "";
  const assignee = params.get("assignee") ?? "";
  const [draft, setDraft] = useState({ state, label, assignee });
  const repository = useForgeRead<Repository>("get_repository", { id });
  const issues = useForgeRead<Items<ForgeRecord>>("list_issues", { repository_id: id, limit: 1000 });
  useRepositoryTrail(repository.data, "Issues");
  const wait = pending(repository, issues);
  if (wait) return wait;
  const r = repository.data!;
  const visible = itemsOf(issues.data).filter((issue) => {
    const labels: string[] = issue.value.labels ?? [];
    const assignees: string[] = issue.value.assignees ?? [];
    return (!state || (issue.value.state ?? "open") === state) && (!label || labels.includes(label)) && (!assignee || assignees.includes(assignee));
  });
  const encoded = encodeURIComponent(id);
  return (
    <ForgePage eyebrow="issue tracker" title="Issues" sub={repositoryName(r)}>
      <ForgeTabs repository={r} current="issues" />
      <form
        className="filters"
        onSubmit={(e) => {
          e.preventDefault();
          const next = new URLSearchParams({ repository_id: id });
          next.set("state", draft.state);
          next.set("label", draft.label);
          next.set("assignee", draft.assignee);
          setParams(next);
        }}
      >
        <label>
          State
          <select value={draft.state} onChange={(e) => setDraft({ ...draft, state: e.target.value })}>
            <option value="">All</option>
            <option value="open">Open</option>
            <option value="closed">Closed</option>
          </select>
        </label>
        <label>
          Label
          <input value={draft.label} onChange={(e) => setDraft({ ...draft, label: e.target.value })} />
        </label>
        <label>
          Assignee
          <input value={draft.assignee} onChange={(e) => setDraft({ ...draft, assignee: e.target.value })} />
        </label>
        <button type="submit">Filter</button>
      </form>
      <div className="issue-list">
        {visible.map((issue) => (
          <article key={issue.id} className="panel issue-row">
            <div>
              <h2>
                <Link to={`/issue?repository_id=${encoded}&id=${encodeURIComponent(issue.id)}`}>{issue.value.title ?? "Untitled issue"}</Link>
              </h2>
              <p className="sub">
                <Identifier value={issue.id} /> · opened {date(issue.created_at)} · assignees <IdentifierList values={issue.value.assignees} />
              </p>
            </div>
            <div>
              <ForgeBadge value={issue.value.state ?? "open"} />
              {(issue.value.labels ?? []).map((item: string) => (
                <ForgeBadge key={item} value={item} />
              ))}
            </div>
          </article>
        ))}
      </div>
      {visible.length === 0 && <div className="panel empty">No issues match these filters.</div>}
    </ForgePage>
  );
}
