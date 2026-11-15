// SPDX-License-Identifier: MIT

/** Change App (lake#1241, docs/ui/apps.md#change-app-split-screen): a split screen over the app page,
 * the conversation with the Application Agent (lake#1240) on the left and the running App in a frame
 * on the right. The divider drags (or moves with the arrow keys) and its place is remembered. Once a
 * confirmed change is applied, the page reads the App until its newest generation is Ready and then
 * reloads the frame, and again over the next minute (SETTLE_RELOADS_MS: Ready can come before the new
 * version answers, lake#1259); Reload and *Open in a new tab* are always there. Close (or Escape) returns to the
 * app page. The App host is a subdomain of the Lake's host, so its sign-in cookie works in the frame. */
import { type PointerEvent as ReactPointerEvent, useCallback, useEffect, useRef, useState } from "react";
import { ExternalLink, Loader2, RefreshCw, X } from "lucide-react";
import { read, type Row } from "@/lib/api";
import { Button, Notice } from "@/lib/ui";
import { AppPhase } from "./AppsPage";
import { AppConversation } from "./AppConversation";
import { deployedAndReady, deployFailed, frameUrl, SETTLE_RELOADS_MS, SPLIT_KEY, splitAt, splitByKey, storedSplit } from "./changeSplit";

function initialSplit(): number {
  try {
    return storedSplit(localStorage.getItem(SPLIT_KEY));
  } catch {
    return storedSplit(null);
  }
}

export function ChangeAppSplit({
  app,
  customerId,
  workspaceId,
  agentDeployed,
  onClose,
}: {
  app: Row;
  customerId: string;
  workspaceId: string | null;
  agentDeployed: boolean;
  onClose: () => void;
}) {
  const appId = String(app.id ?? "");
  const url = frameUrl(app.url);
  const [split, setSplit] = useState(initialSplit);
  const [frameKey, setFrameKey] = useState(0);
  const [waiting, setWaiting] = useState(false);
  // Ready was read on the change; the frame reloads again at each SETTLE_RELOADS_MS step.
  const [settling, setSettling] = useState(false);
  const [failed, setFailed] = useState<string | null>(null);
  const [view, setView] = useState<Row>(app);
  const container = useRef<HTMLDivElement | null>(null);
  const dragging = useRef(false);

  const remember = useCallback((next: number) => {
    setSplit(next);
    try {
      localStorage.setItem(SPLIT_KEY, String(next));
    } catch {
      /* private mode: the split lasts for the page */
    }
  }, []);

  const reload = useCallback(() => setFrameKey((key) => key + 1), []);

  // Escape closes, as the page's other overlays do.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  // A confirmed change was applied: read the App every 2 seconds until it runs the change.
  useEffect(() => {
    if (!waiting) return;
    let live = true;
    const tick = async () => {
      try {
        const next = await read<Row>("get_app", { customer_id: customerId, workspace_id: workspaceId ?? undefined, app_id: appId });
        if (!live) return;
        setView(next);
        if (deployedAndReady(next)) {
          setWaiting(false);
          reload();
          setSettling(true);
          return;
        }
        if (deployFailed(next)) {
          setWaiting(false);
          setFailed(typeof next.message === "string" && next.message ? next.message : "The App did not become ready after the change.");
          return;
        }
      } catch {
        /* read again on the next tick */
      }
      if (live) timer = window.setTimeout(tick, 2000);
    };
    let timer = window.setTimeout(tick, 0);
    return () => {
      live = false;
      window.clearTimeout(timer);
    };
  }, [waiting, customerId, workspaceId, appId, reload]);

  // Ready was read: reload again at each step, since the old version may still be answering.
  useEffect(() => {
    if (!settling) return;
    const timers = SETTLE_RELOADS_MS.map((delay, index) =>
      window.setTimeout(() => {
        reload();
        if (index === SETTLE_RELOADS_MS.length - 1) setSettling(false);
      }, delay),
    );
    return () => timers.forEach((timer) => window.clearTimeout(timer));
  }, [settling, reload]);

  const onApplied = useCallback(() => {
    setFailed(null);
    setSettling(false);
    setWaiting(true);
  }, []);

  const onPointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    dragging.current = true;
    event.currentTarget.setPointerCapture(event.pointerId);
  };
  const onPointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!dragging.current || !container.current) return;
    const box = container.current.getBoundingClientRect();
    remember(splitAt(event.clientX, box.left, box.width));
  };
  const onPointerUp = (event: ReactPointerEvent<HTMLDivElement>) => {
    dragging.current = false;
    event.currentTarget.releasePointerCapture(event.pointerId);
  };

  const name = String(app.name ?? "");
  return (
    <div className="fixed inset-0 z-50 flex flex-col bg-canvas" role="dialog" aria-modal="true" aria-label={`Change ${name}`}>
      <div className="flex flex-wrap items-center gap-3 border-b border-hairline px-4 py-2">
        <h2 className="m-0 text-[15px]">Change {name}</h2>
        <AppPhase phase={view.phase as string | undefined} ready={!!view.ready} />
        {waiting ? (
          <span className="muted inline-flex items-center gap-1.5 text-[12.5px]" role="status">
            <Loader2 size={13} className="animate-spin" aria-hidden /> Deploying the change; the App reloads when it is ready
          </span>
        ) : settling ? (
          <span className="muted inline-flex items-center gap-1.5 text-[12.5px]" role="status">
            <Loader2 size={13} className="animate-spin" aria-hidden /> The change is deployed; the App reloads a few times while the new version takes over. Reload if it still shows the old one.
          </span>
        ) : null}
        <span className="ml-auto inline-flex flex-wrap gap-2">
          <Button onClick={reload} disabled={!url} aria-label="Reload the App">
            <RefreshCw size={14} aria-hidden /> Reload
          </Button>
          {url ? (
            <a className="button" href={url} target="_blank" rel="noopener noreferrer">
              <ExternalLink size={14} aria-hidden /> Open in a new tab
            </a>
          ) : null}
          <Button variant="ghost" onClick={onClose} aria-label="Close Change App">
            <X size={14} aria-hidden /> Close
          </Button>
        </span>
      </div>
      <div ref={container} className="flex min-h-0 flex-1">
        <section className="flex min-h-0 min-w-0 flex-col p-4" style={{ width: `${split * 100}%` }} aria-label="Conversation">
          <AppConversation appId={appId} customerId={customerId} workspaceId={workspaceId} agentDeployed={agentDeployed} onApplied={onApplied} className="flex-1" />
        </section>
        <div
          role="separator"
          aria-orientation="vertical"
          aria-label="Resize the conversation and the App"
          aria-valuemin={25}
          aria-valuemax={75}
          aria-valuenow={Math.round(split * 100)}
          tabIndex={0}
          className="w-1.5 shrink-0 cursor-col-resize bg-hairline hover:bg-action focus-visible:bg-action"
          onPointerDown={onPointerDown}
          onPointerMove={onPointerMove}
          onPointerUp={onPointerUp}
          onKeyDown={(event) => {
            const next = splitByKey(split, event.key, event.shiftKey);
            if (next === null) return;
            event.preventDefault();
            remember(next);
          }}
        />
        <section className="flex min-h-0 min-w-0 flex-1 flex-col" aria-label="The running App">
          {failed ? (
            <div className="p-3">
              <Notice tone="danger" title="The change did not come up">
                {failed}
              </Notice>
            </div>
          ) : null}
          {url ? (
            // The App's own origin, a subdomain of this Lake; no sandbox, because the App needs its
            // scripts and its sign-in cookie, exactly as in its own tab.
            <iframe key={frameKey} src={url} title={`${name}, running`} className="min-h-0 w-full flex-1 border-0 bg-white" data-reloads={frameKey} />
          ) : (
            <div className="p-4">
              <Notice tone="warning">This App has no address yet, so it cannot be shown. It gets one once it is deployed.</Notice>
            </div>
          )}
        </section>
      </div>
    </div>
  );
}
