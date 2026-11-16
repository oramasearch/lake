// SPDX-License-Identifier: MIT

/** A secret's value as the pages edit it (docs/ui/secrets.md): `put_secret` takes a JSON object,
 * written either as key/value rows or as raw JSON. Pure, so `npm test` checks it. Nothing here
 * stores, logs or puts a value in a URL. */

export type ValueRow = { key: string; value: string };

export type Parsed = { ok: true; value: Record<string, unknown> } | { ok: false; message: string; row?: number };

/** Rows → the object `put_secret` takes. A row with neither a key nor a value is skipped; a value
 * without a key, or a key used twice, is refused, naming the row. */
export function rowsToValue(rows: ValueRow[]): Parsed {
  const value: Record<string, unknown> = {};
  let filled = 0;
  for (let index = 0; index < rows.length; index++) {
    const { key, value: text } = rows[index];
    const name = key.trim();
    if (!name && text === "") continue;
    if (!name) return { ok: false, message: `Row ${index + 1} has a value but no key.`, row: index };
    if (Object.prototype.hasOwnProperty.call(value, name)) return { ok: false, message: `The key "${name}" is used twice.`, row: index };
    value[name] = text;
    filled++;
  }
  if (filled === 0) return { ok: false, message: "Add at least one key and value." };
  return { ok: true, value };
}

/** Raw JSON → the object `put_secret` takes: it must be one JSON object. */
export function parseJsonObject(text: string, what = "The value"): Parsed {
  if (!text.trim()) return { ok: false, message: `${what} is empty: write a JSON object such as {"password": "…"}.` };
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch (error) {
    return { ok: false, message: `${what} is not valid JSON: ${(error as Error).message}` };
  }
  if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) return { ok: false, message: `${what} must be a JSON object, in braces.` };
  return { ok: true, value: parsed as Record<string, unknown> };
}

/** An object → rows, when every field is a string (rows edit strings only); null otherwise. */
export function valueToRows(value: Record<string, unknown>): ValueRow[] | null {
  const rows: ValueRow[] = [];
  for (const [key, field] of Object.entries(value)) {
    if (typeof field !== "string") return null;
    rows.push({ key, value: field });
  }
  return rows;
}

/** A field of a revealed value as text: strings as they are, anything else as JSON. */
export function fieldText(field: unknown): string {
  return typeof field === "string" ? field : JSON.stringify(field);
}

/** The state of a version, from its deletion and destruction instants. */
export type VersionState = "active" | "deleted" | "destroyed";
export function versionState(version: { deleted_at?: string | null; destroyed_at?: string | null }): VersionState {
  return version.destroyed_at ? "destroyed" : version.deleted_at ? "deleted" : "active";
}

/** UTF-8 text → base64url without padding, the form encrypt, sign and hmac take. */
export function toBase64Url(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

/** base64url (padded or not) → UTF-8 text; null when it is not base64url or not UTF-8. */
export function fromBase64Url(value: string): string | null {
  const clean = value.trim().replace(/-/g, "+").replace(/_/g, "/");
  if (!/^[A-Za-z0-9+/]*=*$/.test(clean)) return null;
  try {
    const binary = atob(clean.padEnd(clean.length + ((4 - (clean.length % 4)) % 4), "="));
    const bytes = Uint8Array.from(binary, (c) => c.charCodeAt(0));
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    return null;
  }
}
