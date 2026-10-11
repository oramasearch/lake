/** New issue (create_issue), in a drawer (lake#598): a title, a Markdown body with a preview, labels
 * and assignees among the repository's people. On success the page opens the new issue. */
import { useState } from "react";
import { Drawer } from "@/lib/overlay";
import { Button, Field, FormError, fieldError } from "@/lib/ui";
import { forgeAct, type ForgeRecord } from "./forge";
import { AssigneePicker, LabelPicker, MarkdownEditor } from "./IssueParts";
import type { Person } from "./issues";

export function NewIssue({
  repositoryId,
  known,
  people,
  viewer,
  onClose,
  onCreated,
}: {
  repositoryId: string;
  known: string[];
  people: Person[];
  viewer?: string;
  onClose: () => void;
  onCreated: (issue: ForgeRecord) => void;
}) {
  const [title, setTitle] = useState("");
  const [body, setBody] = useState("");
  const [labels, setLabels] = useState<string[]>([]);
  const [assignees, setAssignees] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const ready = !!title.trim();

  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Record<string, unknown> = { repository_id: repositoryId, title: title.trim() };
      if (body.trim()) input.body = body;
      if (labels.length) input.labels = labels;
      if (assignees.length) input.assignees = assignees;
      onCreated(await forgeAct<ForgeRecord>("create_issue", input));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Drawer
      title="New Issue"
      subtitle="Track a bug, a task or a question on this repository."
      onClose={onClose}
      width={620}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-issue" variant="primary" loading={busy} disabled={!ready}>
            Open issue
          </Button>
        </>
      }
    >
      <form
        id="new-issue"
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          if (ready) void submit();
        }}
      >
        {!["title", "body", "labels", "assignees"].some((field) => fieldError(error, field)) && <FormError error={error} />}
        <Field label="Title" required error={fieldError(error, "title")}>
          <input value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Import fails on an empty CSV" autoFocus />
        </Field>
        <Field label="Description" error={fieldError(error, "body")}>
          <MarkdownEditor label="Description" value={body} onChange={setBody} placeholder="What happened, what you expected, and the steps to see it." />
        </Field>
        <Field label="Labels" error={fieldError(error, "labels")}>
          <LabelPicker value={labels} onChange={setLabels} known={known} />
        </Field>
        <Field label="Assignees" help="People of the repository's workspace." error={fieldError(error, "assignees")}>
          <AssigneePicker value={assignees} onChange={setAssignees} people={people} viewer={viewer} />
        </Field>
      </form>
    </Drawer>
  );
}
