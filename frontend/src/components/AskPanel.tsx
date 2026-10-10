/** Ask Orama beside every page, replacing the server-rendered dock. `from` is the page it was opened
 * from (path and query), which Ask Orama is told about. Ported with the Ask page. */
export function AskPanel({ onClose, from }: { onClose: () => void; from: string }) {
  return (
    <aside className="ask-panel" aria-label="Ask Orama" data-from={from}>
      <button type="button" onClick={onClose}>
        Close
      </button>
    </aside>
  );
}
