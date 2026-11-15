// SPDX-License-Identifier: MIT

/** A search hit as a person reads it (zetta#1243, lake#596): the matched row's fields, in the
 * index's own field order, rather than the workspace's internal row id. No imports, so `npm test`
 * runs it directly. */

/** One field's value as text: a string as it is, null or absent as "—", anything else as JSON. */
export function fieldText(value: unknown): string {
  if (value === null || value === undefined) return "—";
  if (typeof value === "string") return value;
  return JSON.stringify(value);
}

/** A hit's fields as [name, text] pairs: the index's fields first, in its order, then any other
 * field the hit carries. [] for a hit with no fields (an older engine, or `fields: []`). */
export function hitFields(hit: unknown, order: readonly string[] = []): [string, string][] {
  const fields = hit && typeof hit === "object" ? (hit as Record<string, unknown>).fields : undefined;
  if (!fields || typeof fields !== "object" || Array.isArray(fields)) return [];
  const values = fields as Record<string, unknown>;
  const names = [...order.filter((name) => name in values), ...Object.keys(values).filter((name) => !order.includes(name))];
  return names.map((name) => [name, fieldText(values[name])]);
}
