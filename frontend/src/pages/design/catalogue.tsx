// SPDX-License-Identifier: MIT

/** What Solutions, Solution, Methodologies and Methodology share: methodology links and chips, the
 * catalogue's toolbar filter, and the "engine does not list solutions yet" note. */
import { Fragment } from "react";
import { Link } from "react-router-dom";
import { BookOpen } from "lucide-react";
import { Button, Empty } from "@/lib/ui";
import { MethodologyIcon } from "@/components/CatalogueIcon";
import { qs, useWq, useWsPath } from "./api";
import { solutionMethodologies, type J } from "./logic";

/** A solution's methodologies as links, each with its minimum level when above 1, or "—". */
export function MethodologyLinks({ solution }: { solution: J }) {
  const ws = useWsPath();
  const list = solutionMethodologies(solution);
  if (!list.length) return <>—</>;
  return (
    <>
      {list.map((m, index) => (
        <Fragment key={`${m.id}-${index}`}>
          {index > 0 && ", "}
          <span className="inline-flex items-center gap-1 align-middle">
            <MethodologyIcon entry={{ id: m.id }} size={20} />
            <Link to={ws(`/design/methodology?${qs({ id: m.id })}`)}>{m.id}</Link>
          </span>
          {m.level > 1 && <span className="sub"> (level {m.level}+)</span>}
        </Fragment>
      ))}
    </>
  );
}

/** A solution's methodologies as small chips (a catalogue card's footer): the id, and its minimum
 * level when above 1. */
export function MethodologyChips({ solution }: { solution: J }) {
  const list = solutionMethodologies(solution);
  if (!list.length) return null;
  return (
    <div className="flex flex-wrap gap-1">
      {list.map((m, index) => (
        <span key={`${m.id}-${index}`} className="inline-flex items-center rounded-full border border-rule bg-surface px-2 py-[1px] font-mono text-[11px] leading-4 text-ink-2">
          {m.id}
          {m.level > 1 && <span className="ml-1 text-ink-3">L{m.level}+</span>}
        </span>
      ))}
    </div>
  );
}

/** Each methodology's family, from the engine's list (empty on an older engine), so a solution of
 * your own built on a methodology the icon set has not drawn takes that family's icon (lake#639). */
export function useMethodologyFamilies(enabled = true): Record<string, string> {
  const listed = useWq<J>("list_methodologies", {}, { retry: false, enabled });
  const families: Record<string, string> = {};
  if (!listed.isError && Array.isArray(listed.data?.methodologies)) for (const m of listed.data!.methodologies as J[]) if (m?.id) families[String(m.id)] = String(m.family ?? "");
  return families;
}

/** Whether an entry matches a catalogue search: its id, name and the text fields given. */
export function matches(entry: J, search: string, fields: string[]): boolean {
  const needle = search.trim().toLowerCase();
  if (!needle) return true;
  return fields.some((field) => {
    const v = entry?.[field];
    return (typeof v === "string" ? v : Array.isArray(v) ? v.map((x) => (typeof x === "string" ? x : JSON.stringify(x))).join(" ") : "").toLowerCase().includes(needle);
  });
}

export function NoCatalogue() {
  const ws = useWsPath();
  return (
    <Empty
      icon={BookOpen}
      title="No Solutions Catalogue Yet"
      action={<Button to={ws("/design/methodologies")}>See Methodologies</Button>}
    >
      This workspace's engine does not list solutions yet. The solutions catalogue arrives with the next engine update.
    </Empty>
  );
}
