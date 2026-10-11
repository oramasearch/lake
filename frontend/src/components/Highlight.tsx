/** `text` with the first case-insensitive occurrence of `q` marked, as the Docs reader and Search
 * show a match. */
import { splitAround } from "@/pages/developer/docs";

export function Highlight({ text, q }: { text: string; q: string }) {
  const parts = splitAround(text, q);
  if (!parts) return <>{text}</>;
  return (
    <>
      {parts[0]}
      <mark className="docs-mark">{parts[1]}</mark>
      {parts[2]}
    </>
  );
}
