/** A run report's "Most at Risk" evidence in words (lake#581). The engine writes each at-risk row's
 * `evidence` as the run's columns beyond the fixed ones (the population's strongest drivers, the
 * same on every row) plus `reason_codes`, the row's top reasons: `[{reason, effect}]`, where
 * `effect` is the feature's signed contribution to that row's risk. The page shows the values as
 * "feature = value" chips and the reasons with their effect. Pure, so `tests/evidence.test.ts`
 * runs it under Node. */

export type EvidenceValue = { name: string; value: string };
export type EvidenceReason = { reason: string; effect: number | null; label: string };
export type Evidence = { values: EvidenceValue[]; reasons: EvidenceReason[] };

/** A number as a person reads it: whole numbers as they are, others to four significant digits. */
export function figure(value: number): string {
  if (!Number.isFinite(value)) return String(value);
  if (Number.isInteger(value)) return value.toLocaleString("en-US");
  return Number(value.toPrecision(4)).toLocaleString("en-US", { maximumFractionDigits: 6 });
}

function shown(value: unknown): string {
  if (value === null || value === undefined) return "—";
  if (typeof value === "number") return figure(value);
  if (typeof value === "boolean") return value ? "yes" : "no";
  if (typeof value === "string") return value;
  return JSON.stringify(value);
}

/** "raises risk by 0.12", "lowers risk by 0.05", or nothing when the effect is unknown or zero. */
export function effectLabel(effect: number | null): string {
  if (effect === null || effect === 0) return "";
  return `${effect > 0 ? "raises" : "lowers"} risk by ${figure(Math.abs(effect))}`;
}

function reasonsOf(value: unknown): EvidenceReason[] {
  let list = value;
  if (typeof list === "string") {
    try {
      list = JSON.parse(list);
    } catch {
      return list ? [{ reason: list as string, effect: null, label: "" }] : [];
    }
  }
  if (!Array.isArray(list)) return [];
  const reasons: EvidenceReason[] = [];
  for (const item of list) {
    if (typeof item === "string") {
      reasons.push({ reason: item, effect: null, label: "" });
    } else if (item && typeof item === "object") {
      const row = item as Record<string, unknown>;
      const reason = String(row.reason ?? row.feature ?? row.name ?? "");
      if (!reason) continue;
      const raw = row.effect ?? row.contribution ?? row.value;
      const effect = typeof raw === "number" && Number.isFinite(raw) ? raw : null;
      reasons.push({ reason, effect, label: effectLabel(effect) });
    }
  }
  return reasons;
}

/** An at-risk row's evidence as values and reasons; anything that is not an object is no evidence. */
export function evidenceOf(evidence: unknown): Evidence {
  let source = evidence;
  if (typeof source === "string") {
    try {
      source = JSON.parse(source);
    } catch {
      return { values: [], reasons: [] };
    }
  }
  if (!source || typeof source !== "object" || Array.isArray(source)) return { values: [], reasons: [] };
  const values: EvidenceValue[] = [];
  let reasons: EvidenceReason[] = [];
  for (const [name, value] of Object.entries(source as Record<string, unknown>)) {
    if (name === "reason_codes") reasons = reasonsOf(value);
    else values.push({ name, value: shown(value) });
  }
  return { values, reasons };
}
