// SPDX-License-Identifier: MIT

/** /change?repository_id=&id= (web_ui_change): a change under review. The header names it, its
 * author, its review state and the diff layout; figures (files, lines, checks); the description and
 * the files changed beside its details, reviews and checks; then every file's diff in a unified or
 * side-by-side layout with its review comments, in lake's forge classes. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { Check, Copy, FileDiff, ShieldCheck } from "lucide-react";
import { Button, Card, Empty, KeyValue, Split, Stat, StatRow } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { useSession } from "@/lib/session";
import { Markdown } from "./Markdown";
import { authorOf } from "./signers";
import {
  ForgePage,
  ForgeStatus,
  Identifier,
  itemsOf,
  octal,
  pending,
  SignerList,
  SignerName,
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
  type WithPeople,
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
  const change = useForgeRead<Change & WithPeople>("get_change", { id, change_id: changeId });
  const { viewer } = useSession();
  const diff = useForgeRead<Items<DiffEntry>>("change_diff", { id, change_id: changeId });
  // Reviews and their comments are forge records in the repository's workspace Zetta (lake#341);
  // the diff does not wait for them, and the page says why they are missing.
  const reviews = useForgeRead<Items<ForgeRecord> & WithPeople>("list_reviews", { repository_id: id, limit: 100 }, { retry: false });
  const comments = useForgeRead<Items<ForgeRecord> & WithPeople>(reviews.isSuccess ? "list_review_comments" : null, { repository_id: id, limit: 1000 }, { retry: false });
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

  const checkList = itemsOf(checks.data);
  const passed = checkList.filter((check) => check.exit_code === 0).length;
  const reviewState = changeReviews[0]?.value.state ?? null;
  return (
    <ForgePage
      title={title || "Untitled change"}
      repository={r}
      tab="changes"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <Identifier value={c.id} />
          <span>
            by <SignerName id={c.author} people={c.people} viewer={viewer.taap_id} /> · <span title={dateTime(c.occurred_at)}>{relative(c.occurred_at)}</span>
          </span>
          {reviewState && <ForgeStatus value={reviewState} />}
        </span>
      }
      actions={
        <>
          <Button onClick={() => void copy()}>
            {copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />} {copied ? "Copied" : "Copy identifier"}
          </Button>
          <div className="view-toggle" role="group" aria-label="Diff layout">
            <button type="button" aria-pressed={view === "unified"} onClick={() => setView("unified")}>
              Unified
            </button>
            <button type="button" aria-pressed={view === "split"} onClick={() => setView("split")}>
              Side by side
            </button>
          </div>
        </>
      }
    >
      <StatRow>
        <Stat label="Files changed" value={entries.length} icon={FileDiff} sub="reformat-only files hidden" />
        <Stat label="Lines added" value={`+${totalAdded}`} tone="ok" sub="across every hunk" />
        <Stat label="Lines removed" value={`−${totalRemoved}`} tone={totalRemoved ? "bad" : undefined} sub="across every hunk" />
        <Stat label="Checks" value={checkList.length ? `${passed}/${checkList.length}` : "0"} icon={ShieldCheck} sub={checkList.length ? "passed" : "none reported"} />
      </StatRow>
      <Split
        side={
          <>
            <Card label="Details">
              <KeyValue
                rows={[
                  ["Author", <SignerName id={c.author} people={c.people} viewer={viewer.taap_id} />],
                  ["Created", dateTime(c.occurred_at)],
                  ["Parents", <Identifier value={c.parents.join(", ")} />],
                  ["Identifier", <Identifier value={c.id} />],
                ]}
              />
            </Card>
            <Card label="Review">
              {unavailable ? (
                <p className="muted m-0">Reviews are unavailable: {unavailable}</p>
              ) : changeReviews.length === 0 ? (
                <p className="muted m-0">No review has been requested.</p>
              ) : (
                <ul className="m-0 list-none space-y-2 p-0">
                  {changeReviews.map((review) => (
                    <li key={review.id}>
                      <div className="flex items-center gap-2">
                        <strong>{review.value.state ?? "open"}</strong> <ForgeStatus value={review.value.state ?? "open"} />
                      </div>
                      <p className="sub m-0">
                        Reviewers: <SignerList values={review.value.reviewers} people={reviews.data?.people} viewer={viewer.taap_id} />
                      </p>
                    </li>
                  ))}
                </ul>
              )}
            </Card>
            <Card label="Checks and attestations">
              {checkList.length === 0 ? (
                <p className="muted m-0">No checks have reported for this change.</p>
              ) : (
                <ul className="m-0 list-none divide-y divide-hairline p-0">
                  {checkList.map((check) => {
                    const outputs = check.outputs.map((output) => output.path);
                    const attested = check.attestations.filter((a) => a.verified).map((a) => a.profile);
                    return (
                      <li key={check.id} className="py-2">
                        <div className="flex flex-wrap items-center gap-2">
                          <strong>{check.action}</strong>
                          <ForgeStatus value={check.exit_code === 0 ? "passed" : "failed"} />
                          <span className="ml-auto">
                            <Identifier value={check.id} />
                          </span>
                        </div>
                        <div className="mt-1 text-[12px] text-ink-2">
                          {check.attestations.length} attestations · outputs: <code>{outputs.length ? outputs.join(", ") : "none"}</code> · attested:{" "}
                          <code>{attested.length ? attested.join(", ") : "none"}</code>
                        </div>
                      </li>
                    );
                  })}
                </ul>
              )}
            </Card>
          </>
        }
      >
        {body && (
          <Card label="Description">
            <Markdown>{body}</Markdown>
          </Card>
        )}
        <Card label="Files changed" padded={false}>
          {entries.length === 0 ? (
            <div className="p-4">
              <Empty compact icon={FileDiff} title="No changes to display" />
            </div>
          ) : (
            <ul className="m-0 list-none divide-y divide-hairline p-0">
              {entries.map((entry, index) => (
                <li key={entry.path + index} className="flex items-center gap-2 px-4 py-2 text-[13px]">
                  <a className="min-w-0 flex-1 truncate" href={`#diff-${slug(entry.path)}`}>
                    <code>{entry.path}</code>
                  </a>
                  {entry.changes.map((status) => (
                    <ForgeStatus key={status} value={status} />
                  ))}
                  <span className="diff-stats font-mono text-[12px]">
                    <span className="additions">+{stats[index].added}</span> <span className="deletions">−{stats[index].removed}</span>
                  </span>
                </li>
              ))}
            </ul>
          )}
        </Card>
      </Split>
      {entries.length > 0 && (
        <section className="page-section">
          <div className="section-heading">
            <h2>Diff</h2>
            <span className="font-mono text-[12px]">
              <span className="additions">+{totalAdded}</span> <span className="deletions">−{totalRemoved}</span>
            </span>
          </div>
          {entries.map((entry, index) => {
            const key = slug(entry.path);
            return (
              <section key={entry.path + index} className="panel diff-entry" id={`diff-${key}`}>
                <h3 className="flex flex-wrap items-center gap-2">
                  <code>{entry.path}</code>
                  {entry.changes.map((status) => (
                    <ForgeStatus key={status} value={status} />
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
                              {authorOf(comment.value.author) && (
                                <>
                                  <SignerName id={authorOf(comment.value.author)} people={comments.data?.people} viewer={viewer.taap_id} /> ·{" "}
                                </>
                              )}
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
        </section>
      )}
    </ForgePage>
  );
}
