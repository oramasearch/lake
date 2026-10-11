/** The top bar's breadcrumb. By default it is the rail group › the rail item › the page title; a
 * page that knows its entities replaces it with `useTrail([...])` (docs/ui/breadcrumb.md). Long
 * hex ids show their first 8 characters, with the whole value on hover. */
import { createContext, type ReactNode, useContext, useEffect, useState } from "react";

export type Crumb = { label: string; to?: string; full?: string };

type TrailState = { trail: Crumb[] | null; title: string | null };
const Ctx = createContext<{ state: TrailState; set: (s: TrailState) => void }>({ state: { trail: null, title: null }, set: () => {} });

export function TrailProvider({ children }: { children: ReactNode }) {
  const [state, set] = useState<TrailState>({ trail: null, title: null });
  return <Ctx.Provider value={{ state, set }}>{children}</Ctx.Provider>;
}

export function useTrailState(): TrailState {
  return useContext(Ctx).state;
}

/** Set this page's breadcrumb (and, from its last level, the document title). */
export function useTrail(trail: Crumb[] | null, title?: string) {
  const { set } = useContext(Ctx);
  const key = JSON.stringify(trail) + (title ?? "");
  useEffect(() => {
    set({ trail, title: title ?? trail?.[trail.length - 1]?.label ?? null });
    return () => set({ trail: null, title: null });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, set]);
}

/** A crumb for an entity: a UUID or a 16+ character hex id shows as its first 8 characters + "…". */
export function entityCrumb(value: string, to?: string): Crumb {
  if (value.length >= 16 && /^[0-9a-fA-F-]+$/.test(value)) return { label: `${value.slice(0, 8)}…`, to, full: value };
  return { label: value, to };
}
