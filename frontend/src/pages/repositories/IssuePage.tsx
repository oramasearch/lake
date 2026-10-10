/** /issue?repository_id=&id= (web_ui_issue): one issue, its discussion and details. lake has no
 * `get_issue`, so the page finds it in `list_issues` as the server did. */
import { useSearchParams } from "react-router-dom";
import { date, dateTime } from "@/lib/format";
import { Markdown } from "./Markdown";
import {
  ErrorPage,
  ForgeBadge,
  ForgePage,
  ForgeTabs,
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

export default function IssuePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  const issueId = params.get("id") ?? "";
  if (!id || !issueId) return <ToRepositories />;
  return <Issue id={id} issueId={issueId} />;
}

function Issue({ id, issueId }: { id: string; issueId: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const issues = useForgeRead<Items<ForgeRecord>>("list_issues", { repository_id: id, limit: 1000 });
  const comments = useForgeRead<Items<ForgeRecord>>("list_issue_comments", { repository_id: id, limit: 1000 });
  useRepositoryTrail(repository.data, "Issue");
  const wait = pending(repository, issues, comments);
  if (wait) return wait;
  const issue = itemsOf(issues.data).find((item) => item.id === issueId);
  if (!issue) return <ErrorPage code="not_found" message="issue not found" />;
  const discussion = itemsOf(comments.data).filter((comment) => comment.value.issue_id === issueId);
  const labels: string[] = issue.value.labels ?? [];
  const assignees: string[] = issue.value.assignees ?? [];
  const references: string[] = issue.value.references ?? [];
  return (
    <ForgePage
      eyebrow="issue"
      title={issue.value.title ?? "Untitled issue"}
      sub={
        <>
          <Identifier value={issue.id} /> opened {date(issue.created_at)} <ForgeBadge value={issue.value.state ?? "open"} />
        </>
      }
    >
      <ForgeTabs repository={repository.data!} current="issues" />
      <div className="issue-layout">
        <div>
          <article className="panel issue-body">
            <Markdown>{String(issue.value.body ?? "")}</Markdown>
          </article>
          <section className="discussion">
            <h2>Discussion</h2>
            {discussion.length === 0 ? (
              <div className="panel empty">No discussion yet.</div>
            ) : (
              discussion.map((comment) => (
                <article key={comment.id} className="panel issue-comment">
                  <p className="sub">
                    <Identifier value={comment.id} /> · {dateTime(comment.created_at)}
                  </p>
                  <Markdown>{String(comment.value.body ?? "")}</Markdown>
                </article>
              ))
            )}
          </section>
        </div>
        <aside className="panel issue-details">
          <h2>Details</h2>
          <dl>
            <dt>labels</dt>
            <dd>{labels.length ? labels.map((label) => <ForgeBadge key={label} value={label} />) : <span className="muted">None</span>}</dd>
            <dt>assignees</dt>
            <dd>{assignees.length ? <IdentifierList values={assignees} /> : <span className="muted">Unassigned</span>}</dd>
          </dl>
          <h3>Linked Changes</h3>
          <ul>
            {references.length ? (
              references.map((reference) => (
                <li key={reference}>
                  <Identifier value={reference} />
                </li>
              ))
            ) : (
              <li className="muted">No linked changes.</li>
            )}
          </ul>
        </aside>
      </div>
    </ForgePage>
  );
}
