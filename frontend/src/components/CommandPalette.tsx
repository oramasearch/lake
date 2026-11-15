// SPDX-License-Identifier: MIT

/** The top bar's search (⌘K or "/", lake#633): Go to (pages and workspaces, instant) and every place
 * Data → Search covers, searched as the person types in every workspace they have. Each place
 * fills in as it answers; a slow one never holds the others, and a failed one says so in its group.
 * ↑ ↓ move across groups, ↵ opens, ⌘↵ (or "See all results") opens Data → Search with the same
 * query and every place on. The places and their reads are `@/lib/federatedPlaces`, shared with
 * Data → Search; the rows, ranking and excerpts are `@/lib/federated` (tests/federated.test.ts). */
import { useEffect, useMemo, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { ArrowRight, LoaderCircle, Search } from "lucide-react";
import { useSession } from "@/lib/session";
import { navigableScreens, sectionLabel, workspacePath } from "@/routes";
import { Overlay } from "@/lib/overlay";
import { Kbd, cx } from "@/lib/ui";
import { Highlight } from "@/components/Highlight";
import { currentSelection, hitWhere, paletteRows, stepSelection, searchAllUrl, selectable, type PaletteGroup } from "@/lib/federated";
import { HitTitleText, PLACES, openHit, unavailable, useDebounced, useFederatedSearch, useSearchScopes } from "@/lib/federatedPlaces";

/** Places are searched from this many characters; Go to filters from the first. */
const MIN_QUERY = 2;

export function CommandPalette({ onClose }: { onClose: () => void }) {
  const navigate = useNavigate();
  const session = useSession();
  const [q, setQ] = useState("");
  const [active, setActive] = useState<string | null>(null);
  const list = useRef<HTMLUListElement>(null);
  const text = q.trim();
  const settled = useDebounced(text, 180);
  const searching = settled.length >= MIN_QUERY ? settled : "";
  const scopes = useSearchScopes();
  const signedIn = !!session.viewer.signed_in;
  const results = useFederatedSearch(signedIn ? searching : "", PLACES, scopes);

  const goTo = useMemo(() => {
    const needle = text.toLowerCase();
    const screens = navigableScreens(session)
      .filter((s) => !needle || s.title.toLowerCase().includes(needle) || sectionLabel(s.section).toLowerCase().includes(needle))
      .slice(0, needle ? 6 : 14)
      .map((s) => ({ key: `s:${s.path}`, label: s.title, hint: sectionLabel(s.section), to: s.workspace ? workspacePath(session.workspaceId, s.path) : s.path }));
    const workspaces = session.workspaces
      .filter((w) => needle && String(w.name ?? "").toLowerCase().includes(needle))
      .slice(0, 4)
      .map((w) => ({ key: `w:${w.id}`, label: String(w.name ?? w.id), hint: "Workspace", to: `/w/${w.id}/` }));
    return [...screens, ...workspaces];
  }, [text, session]);

  const groups = useMemo<PaletteGroup[]>(
    () =>
      PLACES.map((place, i) => {
        const r = results[i];
        const base = { id: place.id, label: place.label };
        if (!signedIn || text.length < MIN_QUERY || (place.workspace && !scopes.length)) return { ...base, status: "off" };
        // Typed but not yet asked (the pause before a search), or asked and not yet answered.
        if (text !== settled || r.isPending) return { ...base, status: "loading" };
        if (r.isError) return unavailable(r.error) ? { ...base, status: "off" } : { ...base, status: "error", error: `Could not search ${place.label.toLowerCase()}.` };
        return { ...base, status: "ok", hits: r.data?.hits, failed: r.data?.failed };
      }),
    [results, signedIn, text, settled, scopes.length],
  );

  const rows = useMemo(() => paletteRows(text, goTo, groups), [text, goTo, groups]);
  const items = selectable(rows);
  const selected = currentSelection(items, active);

  useEffect(() => {
    setActive(null);
  }, [text]);
  useEffect(() => {
    if (!selected) return;
    list.current?.querySelector<HTMLElement>(`[data-key="${CSS.escape(selected)}"]`)?.scrollIntoView({ block: "nearest" });
  }, [selected]);

  const seeAll = () => {
    navigate(searchAllUrl(text));
    onClose();
  };
  const run = (key: string | null) => {
    const item = items.find((i) => i.key === key);
    if (!item) return;
    if (item.key === "see-all") return seeAll();
    if (item.hit) openHit(item.hit, navigate);
    else {
      const page = goTo.find((g) => g.key === item.key);
      if (page) navigate(page.to);
    }
    onClose();
  };

  const optionId = (key: string) => `palette-${items.findIndex((i) => i.key === key)}`;

  return (
    <Overlay onClose={onClose} label="Search">
      <div className="palette" role="dialog" aria-modal aria-label="Search">
        <div className="palette-input">
          <Search size={16} aria-hidden className="shrink-0 text-ink-3" />
          <input
            autoFocus
            value={q}
            aria-label="Search everything, or go to a page"
            role="combobox"
            aria-expanded
            aria-controls="palette-results"
            aria-activedescendant={selected ? optionId(selected) : undefined}
            onChange={(e) => setQ(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                e.preventDefault();
                setActive(stepSelection(items, active, e.key === "ArrowDown" ? 1 : -1));
              } else if (e.key === "Enter") {
                e.preventDefault();
                if ((e.metaKey || e.ctrlKey) && text) seeAll();
                else run(selected);
              }
            }}
            placeholder="Search everything, or go to a page…"
          />
          <button type="button" className="palette-close" onClick={onClose} aria-label="Close">
            <Kbd>esc</Kbd>
          </button>
        </div>
        <ul id="palette-results" role="listbox" ref={list} aria-label="Results">
          {rows.map((row) => {
            if (row.kind === "group") {
              const place = PLACES.find((p) => `g:${p.id}` === row.key);
              return (
                <li key={row.key} role="presentation" className="palette-group">
                  {place && <place.icon size={12} aria-hidden />}
                  <span className="flex-1">{row.label}</span>
                  {row.state === "loading" && <LoaderCircle size={12} aria-label="Searching" className="animate-spin" />}
                  {row.state === "count" && <span className="font-mono">{row.count}</span>}
                </li>
              );
            }
            if (row.kind === "note") {
              return (
                <li key={row.key} role="presentation" className="palette-note">
                  {row.text}
                </li>
              );
            }
            const on = row.key === selected;
            const common = {
              id: optionId(row.key),
              role: "option" as const,
              "aria-selected": on,
              "data-key": row.key,
              onMouseMove: () => !on && setActive(row.key),
              onClick: () => run(row.key),
            };
            if (row.key === "see-all") {
              return (
                <li key={row.key} {...common} className="palette-item palette-all">
                  <span className="inline-flex min-w-0 items-center gap-2">
                    <Search size={14} aria-hidden className="shrink-0" />
                    <span className="truncate">{row.label}</span>
                  </span>
                  <small className="hidden sm:inline">
                    <Kbd>⌘</Kbd> <Kbd>↵</Kbd>
                  </small>
                </li>
              );
            }
            if (row.hit) {
              const where = hitWhere(row.hit, text);
              const Icon = row.hit.icon as typeof Search | undefined;
              return (
                <li key={row.key} {...common} className="palette-item palette-hit">
                  <span className="palette-title">
                    {Icon && <Icon size={13} aria-hidden className={cx("shrink-0", row.hit.iconClass)} />}
                    <span className="truncate">
                      <HitTitleText hit={row.hit} text={text} />
                    </span>
                  </span>
                  {where && (
                    <span className={cx("palette-where", row.hit.detailCode && "font-mono")}>
                      <Highlight text={where} q={text} />
                    </span>
                  )}
                </li>
              );
            }
            return (
              <li key={row.key} {...common} className="palette-item">
                <span className="inline-flex min-w-0 items-center gap-2">
                  <ArrowRight size={13} aria-hidden className="shrink-0 text-ink-3" />
                  <span className="truncate">
                    <Highlight text={row.label ?? ""} q={text} />
                  </span>
                </span>
                <small>{row.hint}</small>
              </li>
            );
          })}
          {!rows.length && <li className="empty">Nothing matches.</li>}
        </ul>
        <div className="dialog-foot palette-foot justify-start text-[var(--lake-text-12)]">
          <span className="muted">
            <Kbd>↑</Kbd> <Kbd>↓</Kbd> move · <Kbd>↵</Kbd> open · <Kbd>⌘</Kbd>
            <Kbd>↵</Kbd> all results · <Kbd>esc</Kbd> close
          </span>
        </div>
      </div>
    </Overlay>
  );
}
