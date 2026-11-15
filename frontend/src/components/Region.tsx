// SPDX-License-Identifier: MIT

/** Regions in the web app (lake#757, docs/ui/regions.md), read from `server_info`: the top bar's
 * region (its label, and a link to each other region's host), and the sentence on the sign-in and
 * sign-up pages. A Lake with no region renders nothing. */
import { useEffect, useRef, useState } from "react";
import { useLocation } from "react-router-dom";
import { Globe } from "lucide-react";
import { useRead } from "@/lib/api";
import { accountRegionSentence, currentRegion, orderSheetLinks, otherRegions, regionUrl, type ServerRegions } from "@/lib/regions";

/** `server_info`'s region fields: one read, shared with every other `server_info` reader. */
export function useServerRegions(): ServerRegions | undefined {
  return useRead<ServerRegions>("server_info", undefined, { workspace: null, staleTime: Infinity }).data;
}

/** The top bar's region: its label, opening a menu of the other regions' hosts. */
export function RegionMenu() {
  const info = useServerRegions();
  const own = currentRegion(info);
  const others = otherRegions(info);
  const order = orderSheetLinks(info);
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const location = useLocation();
  useEffect(() => {
    setOpen(false);
  }, [location.pathname]);
  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => {
      if (!box.current?.contains(e.target as Node)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", key);
    };
  }, [open]);
  if (!own) return null;
  const label = (
    <>
      <Globe size={14} aria-hidden />
      <span className="hidden sm:inline">{own.label}</span>
    </>
  );
  const className = "hit inline-flex h-8 shrink-0 items-center gap-1.5 rounded-[5px] px-2 text-[12.5px] text-ink-2 hover:bg-raised hover:text-ink";
  if (!others.length && !order) {
    return (
      <span className={className} title={`Region: ${own.label}`} aria-label={`Region: ${own.label}`}>
        {label}
      </span>
    );
  }
  return (
    <div ref={box} className="relative">
      <button type="button" onClick={() => setOpen((o) => !o)} aria-haspopup="menu" aria-expanded={open} aria-label={`Region: ${own.label}`} title={`Region: ${own.label}`} className={className}>
        {label}
      </button>
      {open && (
        <div role="menu" className="absolute top-full right-0 z-50 mt-1 w-[240px] overflow-hidden rounded-[6px] border border-hairline bg-overlay py-1 shadow-popover">
          <div className="px-3 py-1.5 text-[12px] text-ink-3">You are in {own.label}. Each region is a separate Lake with its own accounts.</div>
          <div className="my-1 border-t border-hairline" />
          {others.map((region) => (
            <a key={region.name} role="menuitem" href={regionUrl(region)} className="block px-3 py-1.5 text-[13px] text-ink no-underline hover:bg-raised">
              {region.label} <span className="font-mono text-[11.5px] text-ink-3">{region.host}</span>
            </a>
          ))}
          {order && (
            <>
              <div className="my-1 border-t border-hairline" />
              <a role="menuitem" href={order.newSetup} data-order-sheet="new-setup" className="block px-3 py-1.5 text-[13px] text-ink no-underline hover:bg-raised">
                New setup…
              </a>
              <a role="menuitem" href={order.onPrem} data-order-sheet="on-prem" className="block px-3 py-1.5 text-[13px] text-ink no-underline hover:bg-raised">
                On-prem…
              </a>
            </>
          )}
        </div>
      )}
    </div>
  );
}

/** The sign-in or sign-up page's sentence naming the region the account is in, with the other
 * regions' hosts. */
export function AccountRegionNote({ mode }: { mode: "sign-in" | "sign-up" }) {
  const info = useServerRegions();
  const sentence = accountRegionSentence(info, mode);
  if (!sentence) return null;
  const others = otherRegions(info);
  return (
    <p className="muted" data-region-note>
      {sentence}
      {others.length > 0 && (
        <>
          {" "}
          {mode === "sign-up" ? "To create it elsewhere, go to " : "Your account is in another region? Go to "}
          {others.map((region, i) => (
            <span key={region.name}>
              {i > 0 && (i === others.length - 1 ? " or " : ", ")}
              <a href={regionUrl(region)}>
                {region.label} ({region.host})
              </a>
            </span>
          ))}
          .
        </>
      )}
    </p>
  );
}

/** The order sheet's two entries as links (lake#831), for a page with a region, such as Workspaces:
 * "New setup…" and "On-prem…". Nothing where this Lake has no order sheet. */
export function OrderSheetLinks() {
  const order = orderSheetLinks(useServerRegions());
  if (!order) return null;
  return (
    <span className="inline-flex items-center gap-3 text-[12.5px]" data-order-sheet-links>
      <a href={order.newSetup}>New setup…</a>
      <a href={order.onPrem}>On-prem…</a>
    </span>
  );
}
