/** Who signed a change, a review or a comment, or holds access (lake#1326): the person's name and
 * handle from the `people` member of the read that named them, with the key on hover. An identity
 * no account acts for reads "Unknown signer" with a short key. `npm test` covers it
 * (tests/signers.test.ts). */

/** One account, as a read's `people` member names it. */
export type Signer = { kind: string; name: string; handle: string };
/** `people`: the TAAP identifier mapped to its account. */
export type People = Record<string, Signer>;

/** The short form of a key a page shows beside "Unknown signer": the algorithm's prefix dropped
 * (`taap:1:ed25519:`), then the first 8 and last 4 characters. */
export function shortKey(id: string): string {
  const key = id.replace(/^taap:\d+:[a-z0-9-]+:/i, "");
  return key.length <= 14 ? key : `${key.slice(0, 8)}…${key.slice(-4)}`;
}

export type SignerLabel = {
  /** What a person reads: the name, else the handle, else "Unknown signer". */
  name: string;
  /** `@handle`, or the short key when the identity is unknown. */
  detail: string;
  /** Whether an account acts for the identity. */
  known: boolean;
  /** The whole key, for the hover. */
  key: string;
};

/** How an identity reads. The viewer's own identity says "(you)". A principal that is not a TAAP
 * identity (an App, a build, a share link, `admin`) reads as itself. */
export function signerLabel(id: string | null | undefined, people: People | undefined, viewer?: string): SignerLabel {
  const key = id ?? "";
  const person = key ? people?.[key] : undefined;
  if (!person && key && !key.startsWith("taap:")) return { name: key, detail: "", known: false, key };
  if (!person) return { name: "Unknown signer", detail: shortKey(key), known: false, key };
  const handle = person.handle ? `@${person.handle}` : "";
  const name = person.name || handle || "Unknown signer";
  return { name: key && key === viewer ? `${name} (you)` : name, detail: person.name ? handle : "", known: true, key };
}

/** The label as one line of text, for a search or a plain string. */
export function signerText(id: string | null | undefined, people: People | undefined, viewer?: string): string {
  const label = signerLabel(id, people, viewer);
  return label.detail ? `${label.name} ${label.detail}` : label.name;
}

/** A comment's author: a TAAP identifier, or liquid's actor object with `taap_id`. */
export function authorOf(value: unknown): string {
  if (typeof value === "string") return value;
  if (value && typeof value === "object" && typeof (value as { taap_id?: unknown }).taap_id === "string") return (value as { taap_id: string }).taap_id;
  return "";
}
