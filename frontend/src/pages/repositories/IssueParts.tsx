// SPDX-License-Identifier: MIT

/** What the issue pages share (lake#598): whether the viewer may write the repository's issues
 * (`my_permissions`, `write` on the repository: what `create_issue`, `update_issue` and
 * `create_issue_comment` check), the people an issue can be assigned to (`issue_assignees`),
 * a label chip, a Markdown editor with a preview, and the label and assignee pickers. */
import { useState } from "react";
import { Check, Plus } from "lucide-react";
import { useSession } from "@/lib/session";
import { Chip, cx, StateTabs } from "@/lib/ui";
import { Markdown } from "./Markdown";
import { useForgeRead } from "./forge";
import { labelTone, personName, toggle, writeReason, type Person } from "./issues";

/** Whether the viewer may open, comment on and change this repository's issues, and why not. */
export function useIssueAccess(repositoryId: string) {
  const { viewer } = useSession();
  const asked = viewer.signed_in && !viewer.administrator;
  const q = useForgeRead<{ allowed?: Record<string, boolean> }>(
    asked ? "my_permissions" : null,
    { object_type: "repository", object_id: repositoryId, actions: JSON.stringify(["write"]) },
    { staleTime: 60_000 },
  );
  const canWrite = viewer.signed_in && (viewer.administrator || !!q.data?.allowed?.write);
  return { canWrite, reason: writeReason(viewer.signed_in, canWrite, asked && q.isLoading), viewer: viewer.taap_id };
}

/** The repository's people, read only for someone who may assign (the capability needs `write`). */
export function useAssignable(repositoryId: string, canWrite: boolean): Person[] {
  const q = useForgeRead<{ people: Person[] }>(canWrite ? "issue_assignees" : null, { repository_id: repositoryId }, { staleTime: 60_000 });
  return q.data?.people ?? [];
}

export function LabelChip({ label, onClick, active }: { label: string; onClick?: () => void; active?: boolean }) {
  return (
    <Chip tone={labelTone(label)} onClick={onClick} active={active}>
      {label}
    </Chip>
  );
}

/** A person's name, the full identity on hover. */
export function PersonName({ id, people, viewer }: { id: string | undefined; people: Person[]; viewer?: string }) {
  if (!id) return <span className="muted">Unknown</span>;
  return (
    <span className="font-semibold text-ink" title={id}>
      {personName(id, people, viewer)}
    </span>
  );
}

/** Markdown with Write and Preview tabs. */
export function MarkdownEditor({
  value,
  onChange,
  label,
  placeholder,
  rows = 8,
  autoFocus,
}: {
  value: string;
  onChange: (value: string) => void;
  label: string;
  placeholder?: string;
  rows?: number;
  autoFocus?: boolean;
}) {
  const [tab, setTab] = useState<"write" | "preview">("write");
  return (
    <div className="w-full min-w-0">
      <StateTabs
        label={`${label}: write or preview`}
        value={tab}
        onChange={setTab}
        tabs={[
          { value: "write", label: "Write" },
          { value: "preview", label: "Preview" },
        ]}
      />
      {tab === "write" ? (
        <textarea aria-label={label} className="w-full font-mono text-[13px]" rows={rows} value={value} onChange={(e) => onChange(e.target.value)} placeholder={placeholder} autoFocus={autoFocus} />
      ) : (
        <div className="min-h-[96px] rounded-[6px] border border-hairline bg-surface px-3 py-2">{value.trim() ? <Markdown>{value}</Markdown> : <p className="muted m-0">Nothing to preview.</p>}</div>
      )}
      <small className="muted">Markdown is supported.</small>
    </div>
  );
}

/** Labels: the repository's known labels to toggle, and a new one typed in. */
export function LabelPicker({ value, onChange, known }: { value: string[]; onChange: (labels: string[]) => void; known: string[] }) {
  const [draft, setDraft] = useState("");
  const options = [...known, ...value.filter((label) => !known.includes(label))];
  const add = () => {
    const label = draft.trim();
    if (label && !value.includes(label)) onChange([...value, label]);
    setDraft("");
  };
  return (
    <div className="stack gap-2">
      {options.length > 0 && (
        <div className="flex flex-wrap gap-1.5" role="group" aria-label="Labels">
          {options.map((label) => (
            <LabelChip key={label} label={label} active={value.includes(label)} onClick={() => onChange(toggle(value, label))} />
          ))}
        </div>
      )}
      <div className="flex gap-2">
        <input
          aria-label="New label"
          className="min-w-0 flex-1"
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              e.preventDefault();
              add();
            }
          }}
          placeholder="Add a label, such as bug"
        />
        <button type="button" className="button" onClick={add} disabled={!draft.trim()}>
          <Plus size={14} aria-hidden /> Add
        </button>
      </div>
    </div>
  );
}

/** Assignees: the repository's people, and anyone already assigned who is not among them. */
export function AssigneePicker({ value, onChange, people, viewer }: { value: string[]; onChange: (assignees: string[]) => void; people: Person[]; viewer?: string }) {
  const others = value.filter((id) => !people.some((person) => person.taap_id === id));
  const rows = [...people.map((person) => person.taap_id), ...others];
  if (!rows.length) return <p className="muted m-0">No one to assign yet.</p>;
  return (
    <ul className="m-0 list-none space-y-1 p-0" aria-label="Assignees">
      {rows.map((id) => {
        const person = people.find((p) => p.taap_id === id);
        const on = value.includes(id);
        return (
          <li key={id}>
            <label className={cx("flex cursor-pointer items-center gap-2.5 rounded-[6px] border px-2.5 py-1.5", on ? "border-action bg-action-soft" : "border-hairline bg-surface hover:border-rule")}>
              <input type="checkbox" checked={on} onChange={() => onChange(toggle(value, id))} />
              <span className="min-w-0 flex-1">
                <span className="block truncate text-[13px] font-semibold text-ink">{personName(id, people, viewer)}</span>
                <span className="block truncate text-[12px] text-ink-3">{person ? [person.handle && `@${person.handle}`, person.email, person.kind === "organization" && "organization"].filter(Boolean).join(" · ") : id}</span>
              </span>
              {on && <Check size={14} aria-hidden className="text-action" />}
            </label>
          </li>
        );
      })}
    </ul>
  );
}
