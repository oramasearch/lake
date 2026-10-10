/** /issue?repository_id=&id= (web_ui_issue): one issue, its discussion beside its details and linked
 * changes. lake has no `get_issue`, so the page finds it in `list_issues` as the server did. */
import { Link, useSearchParams } from "react-router-dom";
import { MessageSquare } from "lucide-react";
import { Card, Chip, Empty, KeyValue, Split } from "@/lib/ui";
import { date, dateTime, relative } from "@/lib/format";
import { Markdown } from "./Markdown";
import {
  ErrorPage,
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
  const state = issue.value.state ?? "open";
  const encoded = encodeURIComponent(id);
  return (
    <ForgePage
      title={issue.value.title ?? "Untitled issue"}
      repository={repository.data!}
      tab="issues"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <ForgeStatus value={state} />
          <Identifier value={issue.id} />
          <span>opened {date(issue.created_at)}</span>
          <span>
            · {discussion.length} {discussion.length === 1 ? "comment" : "comments"}
          </span>
        </span>
      }
    >
      <Split
        side={
          <>
            <Card label="Details">
              <KeyValue
                rows={[
                  ["State", <ForgeStatus value={state} />],
                  [
                    "Labels",
                    labels.length ? (
                      <span className="flex flex-wrap gap-1">
                        {labels.map((label) => (
                          <Chip key={label}>{label}</Chip>
                        ))}
                      </span>
                    ) : (
                      <span className="muted">None</span>
                    ),
                  ],
                  ["Assignees", assignees.length ? <IdentifierList values={assignees} /> : <span className="muted">Unassigned</span>],
                  ["Opened", dateTime(issue.created_at)],
                  ["Updated", relative(issue.updated_at)],
                ]}
              />
            </Card>
            <Card label="Linked changes">
              {references.length ? (
                <ul className="m-0 list-none space-y-1.5 p-0">
                  {references.map((reference) => (
                    <li key={reference}>
                      <Link to={`/change?repository_id=${encoded}&id=${encodeURIComponent(reference)}`} className="no-underline">
                        <Identifier value={reference} />
                      </Link>
                    </li>
                  ))}
                </ul>
              ) : (
                <p className="muted m-0">No linked changes.</p>
              )}
            </Card>
          </>
        }
      >
        <Card label="Description">
          {String(issue.value.body ?? "").trim() ? <Markdown>{String(issue.value.body ?? "")}</Markdown> : <p className="muted m-0">No description.</p>}
        </Card>
        <section className="discussion">
          <div className="section-heading">
            <h2>Discussion</h2>
          </div>
          {discussion.length === 0 ? (
            <Empty compact icon={MessageSquare} title="No discussion yet" />
          ) : (
            <div className="space-y-3">
              {discussion.map((comment) => (
                <Card key={comment.id} label={<Identifier value={comment.id} />} actions={<span className="text-[12px] text-ink-3">{dateTime(comment.created_at)}</span>}>
                  <Markdown>{String(comment.value.body ?? "")}</Markdown>
                </Card>
              ))}
            </div>
          )}
        </section>
      </Split>
    </ForgePage>
  );
}
