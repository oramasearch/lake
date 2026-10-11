/** /issue?repository_id=&id= (web_ui_issue): one issue. The conversation (its description and
 * Markdown comments, then a comment box) beside a side column with its state, labels, assignees,
 * details and linked changes. lake has no `get_issue`, so the page finds it in `list_issues` as the
 * server did.
 *
 * A person with `write` on the repository comments (`create_issue_comment`), edits the title and
 * description, changes labels and assignees, and closes or reopens it (`update_issue`, sending only
 * the fields they changed). Anyone else sees the controls disabled with the reason (lake#598). */
import { type ReactNode, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { CircleCheck, CircleDot, MessageSquare, Pencil } from "lucide-react";
import { Banner, Button, Card, Empty, Field, FormError, KeyValue, Split, fieldError } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { useToast } from "@/lib/toast";
import { Markdown } from "./Markdown";
import {
  ErrorPage,
  forgeAct,
  ForgePage,
  ForgeStatus,
  Identifier,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";
import { AssigneePicker, LabelChip, LabelPicker, MarkdownEditor, PersonName, useAssignable, useIssueAccess } from "./IssueParts";
import { issueChanges, issueState, knownLabels, personName, type IssueRecord, type Person } from "./issues";

export default function IssuePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  const issueId = params.get("id") ?? "";
  if (!id || !issueId) return <ToRepositories />;
  return <Issue id={id} issueId={issueId} />;
}

function Issue({ id, issueId }: { id: string; issueId: string }) {
  const toast = useToast();
  const repository = useForgeRead<Repository>("get_repository", { id });
  const issues = useForgeRead<Items<ForgeRecord>>("list_issues", { repository_id: id, limit: 1000 });
  const comments = useForgeRead<Items<ForgeRecord>>("list_issue_comments", { repository_id: id, limit: 1000 });
  const access = useIssueAccess(id);
  const people = useAssignable(id, access.canWrite);
  const [editing, setEditing] = useState(false);
  const [busy, setBusy] = useState<string | null>(null);
  const [stateError, setStateError] = useState<unknown>(null);
  useRepositoryTrail(repository.data, "Issue");
  const wait = pending(repository, issues, comments);
  if (wait) return wait;
  const all = itemsOf(issues.data) as IssueRecord[];
  const issue = all.find((item) => item.id === issueId);
  if (!issue) return <ErrorPage code="not_found" message="issue not found" />;
  // Comments come newest first; a conversation reads oldest first.
  const discussion = itemsOf(comments.data)
    .filter((comment) => comment.value.issue_id === issueId)
    .sort((a, b) => a.created_at.localeCompare(b.created_at));
  const labels: string[] = issue.value.labels ?? [];
  const assignees: string[] = issue.value.assignees ?? [];
  // A reference names a change or a path (`{change}`, `{path}`); older records kept a change id.
  const references = (issue.value.references as (string | { change?: string; path?: string })[] | undefined) ?? [];
  const state = issueState(issue);
  const encoded = encodeURIComponent(id);
  const reason = access.reason ?? undefined;

  /** `update_issue` with the fields that changed. */
  const update = async (changes: Record<string, unknown>) => forgeAct("update_issue", { repository_id: id, issue_id: issueId, ...changes });
  const setState = async (next: "open" | "closed", comment?: string) => {
    setBusy(next);
    setStateError(null);
    try {
      if (comment?.trim()) await forgeAct("create_issue_comment", { repository_id: id, issue_id: issueId, body: comment });
      await update({ state: next });
      toast(next === "closed" ? "Issue closed" : "Issue reopened");
      return true;
    } catch (e) {
      setStateError(e);
      return false;
    } finally {
      setBusy(null);
    }
  };
  const stateButton = (comment?: string, onDone?: () => void) => {
    const closing = state === "open";
    const Icon = closing ? CircleCheck : CircleDot;
    const words = closing ? (comment?.trim() ? "Close with comment" : "Close issue") : comment?.trim() ? "Reopen with comment" : "Reopen issue";
    return (
      <Button
        loading={busy === (closing ? "closed" : "open")}
        disabled={!access.canWrite || !!busy}
        title={reason}
        onClick={() => {
          void setState(closing ? "closed" : "open", comment).then((done) => {
            if (done) onDone?.();
          });
        }}
      >
        <Icon size={14} aria-hidden /> {words}
      </Button>
    );
  };

  return (
    <ForgePage
      title={issue.value.title ?? "Untitled issue"}
      repository={repository.data!}
      tab="issues"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <ForgeStatus value={state} />
          <Identifier value={issue.id} />
          <span>
            opened {relative(issue.created_at)}
            {issue.value.author ? ` by ${personName(issue.value.author, people, access.viewer)}` : ""}
          </span>
          <span>
            · {discussion.length} {discussion.length === 1 ? "comment" : "comments"}
          </span>
        </span>
      }
      actions={
        <Button disabled={!access.canWrite || editing} title={reason} onClick={() => setEditing(true)}>
          <Pencil size={14} aria-hidden /> Edit
        </Button>
      }
    >
      {stateError ? <FormError error={stateError} /> : null}
      <Split
        side={
          <>
            <Card label="State">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <ForgeStatus value={state} />
                {stateButton()}
              </div>
            </Card>
            <LabelsCard id={id} issueId={issueId} labels={labels} known={knownLabels(all)} canWrite={access.canWrite} reason={reason} />
            <AssigneesCard id={id} issueId={issueId} assignees={assignees} people={people} viewer={access.viewer} canWrite={access.canWrite} reason={reason} />
            <Card label="Details">
              <KeyValue
                rows={[
                  ["Opened by", issue.value.author ? <PersonName id={issue.value.author} people={people} viewer={access.viewer} /> : <span className="muted">Not recorded</span>],
                  ["Opened", dateTime(issue.created_at)],
                  ["Updated", relative(issue.updated_at)],
                  ["Identifier", <Identifier value={issue.id} />],
                ]}
              />
            </Card>
            <Card label="References">
              {references.length ? (
                <ul className="m-0 list-none space-y-1.5 p-0">
                  {references.map((value, index) => {
                    if (typeof value === "string" || value?.change) {
                      const change = typeof value === "string" ? value : value.change!;
                      return (
                        <li key={index}>
                          <Link to={`/change?repository_id=${encoded}&id=${encodeURIComponent(change)}`} className="no-underline">
                            <Identifier value={change} />
                          </Link>
                        </li>
                      );
                    }
                    return (
                      <li key={index}>
                        <code>{value?.path ?? ""}</code>
                      </li>
                    );
                  })}
                </ul>
              ) : (
                <p className="muted m-0">No linked changes or paths.</p>
              )}
            </Card>
          </>
        }
      >
        {editing ? (
          <EditIssue id={id} issue={issue} onDone={() => setEditing(false)} />
        ) : (
          <Card title={<Byline author={issue.value.author} at={issue.created_at} people={people} viewer={access.viewer} verb="opened this issue" />}>
            {String(issue.value.body ?? "").trim() ? <Markdown>{String(issue.value.body ?? "")}</Markdown> : <p className="muted m-0">No description.</p>}
          </Card>
        )}
        <section className="discussion">
          <div className="section-heading">
            <h2>Conversation</h2>
          </div>
          {discussion.length === 0 ? (
            <Empty compact icon={MessageSquare} title="No comments yet">
              {access.canWrite ? "Start the conversation below." : "Comments on this issue will appear here."}
            </Empty>
          ) : (
            <div className="space-y-3">
              {discussion.map((comment) => (
                <Card key={comment.id} title={<Byline author={comment.value.author} at={comment.created_at} people={people} viewer={access.viewer} verb="commented" />}>
                  <Markdown>{String(comment.value.body ?? "")}</Markdown>
                </Card>
              ))}
            </div>
          )}
        </section>
        {access.canWrite ? <CommentBox id={id} issueId={issueId} stateButton={stateButton} /> : reason ? <Banner tone="quiet">{reason}</Banner> : null}
      </Split>
    </ForgePage>
  );
}

/** Who wrote it and when: "Ana Lima commented 2 hours ago". */
function Byline({ author, at, people, viewer, verb }: { author?: string; at: string; people: Person[]; viewer?: string; verb: string }) {
  return (
    <span className="text-[13px] font-normal">
      {author ? <PersonName id={author} people={people} viewer={viewer} /> : <span className="text-ink-2">Someone</span>} <span className="text-ink-3">{verb}</span>{" "}
      <span className="font-mono text-[11.5px] text-ink-3" title={dateTime(at)}>
        {relative(at)}
      </span>
    </span>
  );
}

/** The comment box: Markdown with a preview, Comment, and close or reopen with the comment. */
function CommentBox({ id, issueId, stateButton }: { id: string; issueId: string; stateButton: (comment?: string, onDone?: () => void) => ReactNode }) {
  const toast = useToast();
  const [body, setBody] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await forgeAct("create_issue_comment", { repository_id: id, issue_id: issueId, body });
      setBody("");
      toast("Comment added");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Card label="Add a Comment">
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          if (body.trim()) void submit();
        }}
      >
        {!fieldError(error, "body") && <FormError error={error} />}
        <MarkdownEditor label="Comment" value={body} onChange={setBody} rows={5} placeholder="Leave a comment" />
        {fieldError(error, "body") && (
          <small className="field-error" role="alert">
            {fieldError(error, "body")}
          </small>
        )}
        <div className="flex flex-wrap justify-end gap-2">
          {stateButton(body, () => setBody(""))}
          <Button type="submit" variant="primary" loading={busy} disabled={!body.trim()}>
            Comment
          </Button>
        </div>
      </form>
    </Card>
  );
}

/** Edit the title and description. */
function EditIssue({ id, issue, onDone }: { id: string; issue: IssueRecord; onDone: () => void }) {
  const toast = useToast();
  const [title, setTitle] = useState(issue.value.title ?? "");
  const [body, setBody] = useState(String(issue.value.body ?? ""));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const changes = issueChanges({ title: issue.value.title, body: String(issue.value.body ?? "") }, { title, body });
  const save = async () => {
    if (!Object.keys(changes).length) return onDone();
    setBusy(true);
    setError(null);
    try {
      await forgeAct("update_issue", { repository_id: id, issue_id: issue.id, ...changes });
      toast("Issue updated");
      onDone();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  return (
    <Card label="Edit Issue">
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          if (title.trim()) void save();
        }}
      >
        {!["title", "body"].some((field) => fieldError(error, field)) && <FormError error={error} />}
        <Field label="Title" required error={fieldError(error, "title")}>
          <input value={title} onChange={(e) => setTitle(e.target.value)} autoFocus />
        </Field>
        <Field label="Description" error={fieldError(error, "body")}>
          <MarkdownEditor label="Description" value={body} onChange={setBody} rows={10} />
        </Field>
        <div className="flex flex-wrap justify-end gap-2">
          <Button onClick={onDone}>Cancel</Button>
          <Button type="submit" variant="primary" loading={busy} disabled={!title.trim()}>
            Save
          </Button>
        </div>
      </form>
    </Card>
  );
}

/** A side card whose value changes in place: the value, or its picker with Save and Cancel. */
function EditableCard<T>({
  label,
  value,
  canWrite,
  reason,
  view,
  picker,
  save,
}: {
  label: string;
  value: T;
  canWrite: boolean;
  reason?: string;
  view: ReactNode;
  picker: (draft: T, setDraft: (value: T) => void) => ReactNode;
  save: (draft: T) => Promise<unknown>;
}) {
  const [draft, setDraft] = useState<T | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const editing = draft !== null;
  return (
    <Card
      label={label}
      actions={
        !editing && (
          <button type="button" className="icon-button" aria-label={`Change ${label.toLowerCase()}`} title={canWrite ? `Change ${label.toLowerCase()}` : reason} disabled={!canWrite} onClick={() => setDraft(value)}>
            <Pencil size={14} aria-hidden />
          </button>
        )
      }
    >
      {editing ? (
        <div className="stack">
          <FormError error={error} />
          {picker(draft as T, setDraft)}
          <div className="flex justify-end gap-2">
            <Button onClick={() => setDraft(null)}>Cancel</Button>
            <Button
              variant="primary"
              loading={busy}
              onClick={() => {
                setBusy(true);
                setError(null);
                save(draft as T)
                  .then(() => setDraft(null))
                  .catch(setError)
                  .finally(() => setBusy(false));
              }}
            >
              Save
            </Button>
          </div>
        </div>
      ) : (
        view
      )}
    </Card>
  );
}

function LabelsCard({ id, issueId, labels, known, canWrite, reason }: { id: string; issueId: string; labels: string[]; known: string[]; canWrite: boolean; reason?: string }) {
  const toast = useToast();
  return (
    <EditableCard<string[]>
      label="Labels"
      value={labels}
      canWrite={canWrite}
      reason={reason}
      view={
        labels.length ? (
          <span className="flex flex-wrap gap-1">
            {labels.map((label) => (
              <LabelChip key={label} label={label} />
            ))}
          </span>
        ) : (
          <p className="muted m-0">None yet.</p>
        )
      }
      picker={(draft, setDraft) => <LabelPicker value={draft} onChange={setDraft} known={known} />}
      save={async (draft) => {
        const changes = issueChanges({ labels }, { labels: draft });
        if (!Object.keys(changes).length) return;
        await forgeAct("update_issue", { repository_id: id, issue_id: issueId, ...changes });
        toast("Labels updated");
      }}
    />
  );
}

function AssigneesCard({ id, issueId, assignees, people, viewer, canWrite, reason }: { id: string; issueId: string; assignees: string[]; people: Person[]; viewer?: string; canWrite: boolean; reason?: string }) {
  const toast = useToast();
  return (
    <EditableCard<string[]>
      label="Assignees"
      value={assignees}
      canWrite={canWrite}
      reason={reason}
      view={
        assignees.length ? (
          <ul className="m-0 list-none space-y-1 p-0">
            {assignees.map((who) => (
              <li key={who} className="text-[13px]">
                <PersonName id={who} people={people} viewer={viewer} />
              </li>
            ))}
          </ul>
        ) : (
          <p className="muted m-0">No one yet.</p>
        )
      }
      picker={(draft, setDraft) => <AssigneePicker value={draft} onChange={setDraft} people={people} viewer={viewer} />}
      save={async (draft) => {
        const changes = issueChanges({ assignees }, { assignees: draft });
        if (!Object.keys(changes).length) return;
        await forgeAct("update_issue", { repository_id: id, issue_id: issueId, ...changes });
        toast("Assignees updated");
      }}
    />
  );
}
