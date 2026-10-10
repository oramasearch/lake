/** Opening Ask Orama from a page with something to ask about (lake#561): the transcript page's "Ask
 * Orama about this". The shell owns the panel; a page calls `openAsk` and the shell opens it with
 * the request. */

export type AskRequest = {
  /** What the context chip says ("Transcript: Weekly sync"). */
  label: string;
  /** Sent as `send_ask_message`'s `page` with the next message, in place of the page's own path. */
  page: Record<string, string>;
  /** Put in the message box to finish or send. */
  draft?: string;
};

const listeners = new Set<(request: AskRequest) => void>();

export function openAsk(request: AskRequest) {
  listeners.forEach((l) => l(request));
}

export function onOpenAsk(listener: (request: AskRequest) => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}
