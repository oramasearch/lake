/** /change?repository_id=&id= (web_ui_change): a change under review. Its description, reviews,
 * files changed in a unified or side-by-side layout with their review comments, and the checks
 * that reported. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { Check, Copy } from "lucide-react";
import { dateTime } from "@/lib/format";
import { Markdown } from "./Markdown";
import {
  ForgeBadge,
  ForgePage,
  ForgeTabs,
  Identifier,
  IdentifierList,
  itemsOf,
  octal,
  pending,
  repositoryName,
  slug,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type Artifact,
  type Change,
  type DiffEntry,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";

/** `hidden_reformatting_entry`: a modification whose line diff is empty (a reformat). */
function hiddenReformatting(entry: DiffEntry): boolean {
  return entry.changes.length === 1 && entry.changes[0] === "modified" && !!entry.line_hunks && entry.line_hunks.length === 0;
}

export default function ChangePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  const changeId = params.get("id") ?? "";
  if (!id || !changeId) return <ToRepositories />;
  return <ChangeReview id={id} changeId={changeId} />;
}

function ChangeReview({ id, changeId }: { id: string; changeId: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const change = useForgeRead<Change>("get_change", { id, change_id: changeId });
  const diff = useForgeRead<Items<DiffEntry>>("change_diff", { id, change_id: changeId });
  // Reviews and their comments are forge records in the repository's workspace Zetta (lake#341);
  // the diff does not wait for them, and the page says why they are missing.
  const reviews = useForgeRead<Items<ForgeRecord>>("list_reviews", { repository_id: id, limit: 100 }, { retry: false });
  const comments = useForgeRead<Items<ForgeRecord>>(reviews.isSuccess ? "list_review_comments" : null, { repository_id: id, limit: 1000 }, { retry: false });
  const checks = useForgeRead<Items<Artifact>>("list_artifacts", { id, limit: 100 });
  const [view, setView] = useState<"unified" | "split">("unified");
  const [copied, setCopied] = useState(false);
  useRepositoryTrail(repository.data, "Change");
  const wait = pending(repository, change, diff, checks);
  if (wait) return wait;
  if (reviews.isLoading || comments.isLoading) return pending(reviews, ...(reviews.isSuccess ? [comments] : []));

  const r = repository.data!;
  const c = change.data!;
  const newline = c.message.indexOf("\n");
  let title = newline < 0 ? c.message : c.message.slice(0, newline);
  if (title.startsWith("# ")) title = title.slice(2);
  const body = newline < 0 ? "" : c.message.slice(newline + 1);
  const unavailable = reviews.isError ? (reviews.error as Error).message : comments.isError ? (comments.error as Error).message : "";
  const changeReviews = itemsOf(reviews.data).filter((review) => review.value.change_id === changeId);
  const reviewIds = new Set(changeReviews.map((review) => review.id));
  const changeComments = reviews.isSuccess
    ? itemsOf(comments.data).filter((comment) => reviewIds.has(comment.value.review_id) && comment.value.anchor?.change === changeId)
    : [];
  const entries = itemsOf(diff.data).filter((entry) => !hiddenReformatting(entry));
  const stats = entries.map((entry) => ({
    added: (entry.line_hunks ?? []).reduce((sum, hunk) => sum + hunk.added_lines.length, 0),
    removed: (entry.line_hunks ?? []).reduce((sum, hunk) => sum + hunk.removed_lines.length, 0),
  }));
  const totalAdded = stats.reduce((sum, s) => sum + s.added, 0);
  const totalRemoved = stats.reduce((sum, s) => sum + s.removed, 0);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(c.id);
      setCopied(true);
    } catch {
      /* the identifier stays selectable */
    }
  };

  return (
    <ForgePage eyebrow="change review" title={title || "Untitled change"} sub={repositoryName(r)}>
      <ForgeTabs repository={r} current="changes" />
      <div className="change-toolbar">
        <span>
          <Identifier value={c.id} />
        </span>
        <button className="button copy-id" type="button" onClick={() => void copy()}>
          {copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />} {copied ? "Copied" : "Copy identifier"}
        </button>
        <div className="view-toggle" role="group" aria-label="Diff layout">
          <button type="button" aria-pressed={view === "unified"} onClick={() => setView("unified")}>
            Unified
          </button>
          <button type="button" aria-pressed={view === "split"} onClick={() => setView("split")}>
            Side by side
          </button>
        </div>
      </div>
      <div className="panel">
        <dl>
          <dt>author</dt>
          <dd>
            <Identifier value={c.author} />
          </dd>
          <dt>created</dt>
          <dd>{dateTime(c.occurred_at)}</dd>
          <dt>parents</dt>
          <dd>
            <Identifier value={c.parents.join(", ")} />
          </dd>
        </dl>
      </div>
      {body && (
        <section className="panel change-message">
          <h2>Description</h2>
          <Markdown>{body}</Markdown>
        </section>
      )}
      <section className="review-summary">
        <h2>Review</h2>
        {unavailable ? (
          <div className="panel empty">Reviews are unavailable: {unavailable}</div>
        ) : changeReviews.length === 0 ? (
          <div className="panel empty">No review has been requested.</div>
        ) : (
          changeReviews.map((review) => (
            <div key={review.id} className="panel review-state">
              <div>
                <strong>{review.value.state ?? "open"}</strong> <ForgeBadge value={review.value.state ?? "open"} />
              </div>
              <p className="sub">
                Reviewers: <IdentifierList values={review.value.reviewers} />
              </p>
            </div>
          ))
        )}
      </section>
      <section>
        <h2>Files Changed</h2>
        {entries.length > 0 && (
          <div className="panel file-summary">
            <strong>{entries.length} files</strong> <span className="additions">+{totalAdded}</span> <span className="deletions">−{totalRemoved}</span>
            <ul>
              {entries.map((entry, index) => (
                <li key={entry.path + index}>
                  <a href={`#diff-${slug(entry.path)}`}>
                    <code>{entry.path}</code>
                  </a>
                  <span className="diff-stats">
                    <span className="additions">+{stats[index].added}</span> <span className="deletions">−{stats[index].removed}</span>
                  </span>
                </li>
              ))}
            </ul>
          </div>
        )}
        {entries.map((entry, index) => {
          const key = slug(entry.path);
          return (
            <section key={entry.path + index} className="panel diff-entry" id={`diff-${key}`}>
              <h3>
                <code>{entry.path}</code>
                {entry.changes.map((status) => (
                  <ForgeBadge key={status} value={status} />
                ))}
              </h3>
              <dl>
                <dt>kind</dt>
                <dd>{entry.kind}</dd>
                <dt>old path</dt>
                <dd>
                  <code>{entry.old_path || "—"}</code>
                </dd>
                <dt>mode</dt>
                <dd>
                  <code>
                    {octal(entry.old_mode)} → {octal(entry.new_mode)}
                  </code>
                </dd>
                <dt>blobs</dt>
                <dd>
                  <Identifier value={entry.old_blob || "—"} /> → <Identifier value={entry.new_blob || "—"} />
                </dd>
              </dl>
              {(entry.old_conflicts.length > 0 || entry.new_conflicts.length > 0) && (
                <div className="notice">
                  <strong>Conflict values</strong>
                  <br />
                  old: <code>{entry.old_conflicts.join(", ")}</code>
                  <br />
                  new: <code>{entry.new_conflicts.join(", ")}</code>
                </div>
              )}
              {entry.line_hunks && (
                <>
                  {view === "unified" ? (
                    <div className="diff-view unified-diff" data-diff-view="unified">
                      {entry.line_hunks.map((hunk, h) => (
                        <div key={h} className="diff-hunk">
                          <div className="hunk-heading">
                            @@ -{hunk.old_line},{hunk.old_count} +{hunk.new_line},{hunk.new_count} @@
                          </div>
                          {hunk.removed_lines.map((line, i) => {
                            const n = hunk.old_line + i;
                            return (
                              <div key={`o${i}`} className="diff-line diff-remove" id={`${key}-old-${n}`}>
                                <a href={`#${key}-old-${n}`}>{n}</a>
                                <code>-{line}</code>
                              </div>
                            );
                          })}
                          {hunk.added_lines.map((line, i) => {
                            const n = hunk.new_line + i;
                            return (
                              <div key={`n${i}`} className="diff-line diff-add" id={`${key}-new-${n}`}>
                                <a href={`#${key}-new-${n}`}>{n}</a>
                                <code>+{line}</code>
                              </div>
                            );
                          })}
                        </div>
                      ))}
                    </div>
                  ) : (
                    <div className="diff-view split-diff" data-diff-view="split">
                      {entry.line_hunks.map((hunk, h) => (
                        <div key={h}>
                          <div className="hunk-heading">
                            @@ -{hunk.old_line},{hunk.old_count} +{hunk.new_line},{hunk.new_count} @@
                          </div>
                          {Array.from({ length: Math.max(hunk.removed_lines.length, hunk.added_lines.length) }, (_, i) => (
                            <div key={i} className="split-row">
                              <code className="diff-remove">{hunk.removed_lines[i] ?? ""}</code>
                              <code className="diff-add">{hunk.added_lines[i] ?? ""}</code>
                            </div>
                          ))}
                        </div>
                      ))}
                    </div>
                  )}
                  {changeComments
                    .filter((comment) => comment.value.anchor?.path === entry.path)
                    .map((comment) => {
                      const line = Number(comment.value.anchor?.line ?? 0);
                      return (
                        <article key={comment.id} className="review-comment">
                          <p className="sub">
                            <Identifier value={comment.id} /> · <a href={`#${key}-new-${line}`}>line {line}</a>
                          </p>
                          <Markdown>{String(comment.value.body ?? "")}</Markdown>
                        </article>
                      );
                    })}
                </>
              )}
            </section>
          );
        })}
        {entries.length === 0 && <div className="panel empty diff-entry">No changes to display.</div>}
      </section>
      <section>
        <h2>Checks and Attestations</h2>
        {itemsOf(checks.data).length === 0 ? (
          <div className="panel empty">No checks have reported for this change.</div>
        ) : (
          <div className="stack">
            {itemsOf(checks.data).map((check) => {
              const outputs = check.outputs.map((output) => output.path);
              const attested = check.attestations.filter((a) => a.verified).map((a) => a.profile);
              return (
                <article key={check.id} className="panel check-row">
                  <strong>{check.action}</strong>
                  <ForgeBadge value={check.exit_code === 0 ? "passed" : "failed"} />
                  <span>{check.attestations.length} attestations</span>
                  <span>
                    outputs: <code>{outputs.length ? outputs.join(", ") : "none"}</code>
                  </span>
                  <span>
                    attested: <code>{attested.length ? attested.join(", ") : "none"}</code>
                  </span>
                  <span>
                    <Identifier value={check.id} />
                  </span>
                </article>
              );
            })}
          </div>
        )}
      </section>
    </ForgePage>
  );
}
