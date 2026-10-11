/** A conversation keeps its place, ported from rev-os's `components/assistant/follow.tsx`
 * (lake#561): it follows each new answer while the person is at the end, stays put when they have
 * scrolled up to read (a "New reply" chip takes them down), and stays at the end when the keyboard
 * opens and the scroller shrinks. */
import { useCallback, useEffect, useLayoutEffect, useRef, useState, type RefObject } from "react";
import { ArrowDown } from "lucide-react";
import { nearBottom, onConversationGrew } from "@/lib/phone";

/** `key` changes whenever the conversation grows (a question, an answer, the working notice);
 * `empty` while there is nothing to follow. */
export function useFollow(scroller: RefObject<HTMLDivElement | null>, key: string, empty: boolean) {
  const atBottom = useRef(true);
  const asked = useRef(false);
  const [chip, setChip] = useState(false);

  const toLatest = useCallback(
    (smooth = true) => {
      const el = scroller.current;
      if (!el) return;
      el.scrollTo({ top: el.scrollHeight, behavior: smooth ? "smooth" : "auto" });
      atBottom.current = true;
      setChip(false);
    },
    [scroller],
  );

  const onScroll = useCallback(() => {
    const el = scroller.current;
    if (!el) return;
    atBottom.current = nearBottom(el.scrollTop, el.clientHeight, el.scrollHeight);
    if (atBottom.current) setChip(false);
  }, [scroller]);

  // The conversation grew: follow, or offer the chip. A layout effect: before paint, no jump.
  useLayoutEffect(() => {
    if (empty) return;
    const action = onConversationGrew(atBottom.current, asked.current);
    if (action === "follow") toLatest(!asked.current);
    else setChip(true);
    asked.current = false;
  }, [key, empty, toLatest]);

  // The keyboard opening (or the panel resizing) shrinks the scroller: keep the end in view.
  useEffect(() => {
    const el = scroller.current;
    if (!el || typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(() => {
      if (atBottom.current) el.scrollTop = el.scrollHeight;
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [scroller]);

  /** The person just asked: their question brings them to the end. */
  const markAsked = useCallback(() => {
    asked.current = true;
  }, []);

  return { chip, onScroll, toLatest, markAsked };
}

export function NewReplyChip({ show, onClick }: { show: boolean; onClick: () => void }) {
  if (!show) return null;
  return (
    <button
      type="button"
      onClick={onClick}
      className="plain absolute bottom-3 left-1/2 z-10 inline-flex h-10 -translate-x-1/2 items-center gap-1.5 rounded-full border border-action bg-surface px-4 text-[13px] font-medium text-action shadow-popover"
    >
      <ArrowDown size={14} /> New reply
    </button>
  );
}
