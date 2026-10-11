/** ⌘K or "/": jump to any page, workspace or repository. Pages come from the route registry, the
 * rest from the same reads the pages use. */
import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { navigableScreens, sectionLabel, workspacePath } from "@/routes";
import { Overlay } from "@/lib/overlay";
import { Kbd } from "@/lib/ui";

type Item = { key: string; label: string; hint: string; go: () => void };

export function CommandPalette({ onClose }: { onClose: () => void }) {
  const navigate = useNavigate();
  const session = useSession();
  const [q, setQ] = useState("");
  const [index, setIndex] = useState(0);
  const signedIn = !!session.viewer.signed_in;
  const repositories = useList<Row>(signedIn ? "list_repositories" : null, undefined, { workspace: null, pick: "repositories", staleTime: 60_000 });

  const items = useMemo<Item[]>(() => {
    const needle = q.toLowerCase().trim();
    const screens = navigableScreens(session)
      .filter((s) => !needle || s.title.toLowerCase().includes(needle) || sectionLabel(s.section).toLowerCase().includes(needle))
      .slice(0, needle ? 10 : 14)
      .map<Item>((s) => ({
        key: `s:${s.path}`,
        label: s.title,
        hint: sectionLabel(s.section),
        go: () => navigate(s.workspace ? workspacePath(session.workspaceId, s.path) : s.path),
      }));
    const workspaces = session.workspaces
      .filter((w) => needle && String(w.name ?? "").toLowerCase().includes(needle))
      .slice(0, 5)
      .map<Item>((w) => ({
        key: `w:${w.id}`,
        label: String(w.name ?? w.id),
        hint: "Workspace",
        go: () => navigate(`/w/${w.id}/`),
      }));
    const repos = (repositories.data ?? [])
      .filter((r) => needle && String(r.name ?? "").toLowerCase().includes(needle))
      .slice(0, 6)
      .map<Item>((r) => ({
        key: `r:${r.id}`,
        label: String(r.name),
        hint: "Repository",
        go: () => navigate(`/repository?id=${encodeURIComponent(r.id)}`),
      }));
    return [...screens, ...workspaces, ...repos];
  }, [q, session, repositories.data, navigate]);

  useEffect(() => {
    setIndex(0);
  }, [q]);

  const run = (item: Item | undefined) => {
    if (!item) return;
    item.go();
    onClose();
  };

  return (
    <Overlay onClose={onClose} label="Go to">
      <div className="palette" role="dialog" aria-modal aria-label="Go to">
        <input
          autoFocus
          value={q}
          aria-label="Go to a page, workspace or repository"
          role="combobox"
          aria-expanded
          aria-controls="palette-results"
          aria-activedescendant={items[index] ? `palette-${index}` : undefined}
          onChange={(e) => setQ(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "ArrowDown") {
              e.preventDefault();
              setIndex((i) => Math.min(items.length - 1, i + 1));
            } else if (e.key === "ArrowUp") {
              e.preventDefault();
              setIndex((i) => Math.max(0, i - 1));
            } else if (e.key === "Enter") {
              run(items[index]);
            }
          }}
          placeholder="Go to a page, workspace or repository…"
        />
        <ul id="palette-results" role="listbox">
          {items.map((item, i) => (
            <li
              key={item.key}
              id={`palette-${i}`}
              role="option"
              aria-selected={i === index}
              className="palette-item"
              onMouseEnter={() => setIndex(i)}
              onClick={() => run(item)}
            >
              <span>{item.label}</span>
              <small>{item.hint}</small>
            </li>
          ))}
          {!items.length && <li className="empty">Nothing matches.</li>}
        </ul>
        <div className="dialog-foot justify-start text-[var(--lake-text-12)]">
          <span className="muted">
            <Kbd>↑</Kbd> <Kbd>↓</Kbd> move · <Kbd>↵</Kbd> open · <Kbd>esc</Kbd> close
          </span>
        </div>
      </div>
    </Overlay>
  );
}
