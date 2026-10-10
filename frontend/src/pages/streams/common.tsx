/** What the two Streams pages share (web_ui_streams.cpp): the page heading, the trail, the topic
 * labels and a promise-returning confirmation dialog. */
import { type ReactNode, useCallback, useRef, useState } from "react";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Button } from "@/lib/ui";
import { qs, useWorkspaceLink } from "@/pages/apps/shared";

/** A topic's label: the server's display name, else lake's fallback for agent topics. */
export function displayName(topic: string): string {
  if (topic === "agent-events") return "Agent events";
  if (topic.startsWith("agent-tasks.")) return `Agent tasks · ${topic.slice(12)}`;
  if (topic.startsWith("agent-turn.")) return `Agent turn · ${topic.slice(11)}`;
  return topic;
}

/** A payload as pretty JSON when it parses, else as its text. */
export function pretty(payload: unknown): string {
  const raw = typeof payload === "string" ? payload : JSON.stringify(payload);
  try {
    return JSON.stringify(JSON.parse(raw), null, 2);
  } catch {
    return raw;
  }
}

/** Train › Streams, as every Streams page set it. */
export function useStreamsTrail() {
  const ws = useWorkspaceLink();
  const { space } = useSession();
  useTrail([
    { label: "Train", to: ws("/train/runs") },
    { label: "Streams", to: ws("/train/streams") + qs({ customer_id: space?.id }) },
  ]);
}

export const STREAMS_SUB = "Browse retained messages without consuming them. Live reads leave subscriber positions unchanged.";

/** A confirmation asked in words before a change (the server page's `confirm(...)`). */
export function useConfirm(): [ReactNode, (message: string, label?: string) => Promise<boolean>] {
  const [asked, setAsked] = useState<{ message: string; label: string } | null>(null);
  const resolver = useRef<((value: boolean) => void) | null>(null);
  const ask = useCallback((message: string, label = "Confirm") => {
    return new Promise<boolean>((resolve) => {
      resolver.current = resolve;
      setAsked({ message, label });
    });
  }, []);
  const settle = (value: boolean) => {
    resolver.current?.(value);
    resolver.current = null;
    setAsked(null);
  };
  const element = asked ? (
    <Modal
      title="Confirm Action"
      onClose={() => settle(false)}
      width={480}
      footer={
        <>
          <Button onClick={() => settle(false)}>Cancel</Button>
          <Button variant="danger" onClick={() => settle(true)}>
            {asked.label}
          </Button>
        </>
      }
    >
      <p className="m-0">{asked.message}</p>
    </Modal>
  ) : null;
  return [element, ask];
}
