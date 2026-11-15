// SPDX-License-Identifier: MIT

/** The Ask Orama conversation a person is in, per workspace, shared by the side panel and the home
 * (lake#600): which conversation, the turn being followed and who they talk to. Kept for this tab
 * (`sessionStorage`), so closing the panel, moving between pages or opening the home keeps it; every
 * reader of the same workspace hears a change at once. */
import { useCallback, useEffect, useState } from "react";
import { EMPTY_THREAD, type ThreadState } from "./home";

export { EMPTY_THREAD, type ThreadState };

const KEY = "lake-ask-panel:";

function recall(workspaceId: string): ThreadState {
  try {
    const stored = JSON.parse(sessionStorage.getItem(KEY + workspaceId) ?? "null");
    return stored && typeof stored === "object" ? { ...EMPTY_THREAD, ...stored } : EMPTY_THREAD;
  } catch {
    return EMPTY_THREAD;
  }
}

function keep(workspaceId: string, state: ThreadState) {
  try {
    sessionStorage.setItem(KEY + workspaceId, JSON.stringify(state));
  } catch {
    /* storage blocked: the conversation lasts while the page is open */
  }
}

const listeners = new Set<() => void>();
function announce() {
  listeners.forEach((l) => l());
}

export function useThread(workspaceId: string): [ThreadState, (next: Partial<ThreadState>) => void] {
  const [state, setState] = useState<ThreadState>(() => recall(workspaceId));
  useEffect(() => {
    const listener = () => setState(recall(workspaceId));
    listener();
    listeners.add(listener);
    return () => {
      listeners.delete(listener);
    };
  }, [workspaceId]);
  const update = useCallback(
    (next: Partial<ThreadState>) => {
      keep(workspaceId, { ...recall(workspaceId), ...next });
      announce();
    },
    [workspaceId],
  );
  return [state, update];
}
