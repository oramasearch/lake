/**
 * Design → Methodologies with a workspace's own methodologies from Python code beside Orama's
 * (lake#655, docs/api/methodologies.md): who an entry is from, the toolbar's filter, a workspace
 * methodology's validation state in a person's words, what a run says, and the input Train sends.
 * Pure, so tests/methodologies.test.ts runs it without a browser.
 */
import type {J} from "./logic";

export type MethodologySource = "orama"|"workspace";

/** Orama's catalogue (read-only, runs natively in the engine) or the workspace's own (Python). */
export function methodologySource(entry: J|null|undefined): MethodologySource {
    if (entry?.source === "workspace")
        return "workspace";
    return typeof entry?.id === "string" && /^W-[0-9a-f]{8}$/.test(entry.id) ? "workspace"
                                                                             : "orama";
}

/** The badge a methodology card and page carry. */
export function methodologyBadge(entry: J): "By Orama"|"Yours" {
    return methodologySource(entry) === "workspace" ? "Yours" : "By Orama";
}

/** The methodologies the toolbar keeps: by source, by family (Orama's), and by a search. */
export function filterMethodologies(
    entries: J[],
    filter: {source: MethodologySource|null; family : string | null; search : string}): J[] {
    const needle = filter.search.trim().toLowerCase();
    return entries.filter((entry) => {
        if (filter.source && methodologySource(entry) !== filter.source)
            return false;
        if (filter.family && String(entry.family ?? "") !== filter.family)
            return false;
        if (!needle)
            return true;
        const haystack =
            [
                entry.id,
                entry.name,
                entry.family,
                entry.description,
                ...(Array.isArray(entry.outputs) ? entry.outputs : []),
                ...(Array.isArray(entry.metrics) ? entry.metrics : [])
            ].map((v) => String(v ?? "").toLowerCase())
                .join(" ");
        return haystack.includes(needle);
    });
}

export type ValidationState = {
    label: string; tone : "ok" | "warn" | "bad" | "neutral"; detail : string
};

/** Where a workspace methodology stands: usable only once Validate passed for its current code. */
export function validationState(entry: J): ValidationState {
    const validation: J =
        entry?.validation && typeof entry.validation === "object" ? entry.validation : {};
    const status = String(validation.status ?? "not_validated");
    const revision = String(entry?.code?.revision ?? "");
    const current = !validation.revision || validation.revision === revision;
    if (status === "passed" && current)
        return {label : "Usable", tone : "ok", detail : "Validate passed for its current code."};
    if (status === "passed")
        return {
            label : "Validate again",
            tone : "warn",
            detail : "Its code changed since Validate passed."
        };
    if (status === "running" && current)
        return {
            label : "Validating",
            tone : "neutral",
            detail : "Its interface is running in the sandbox on a sample."
        };
    if (status === "failed" && current)
        return {
            label : "Needs fixes",
            tone : "bad",
            detail : "Validate found something missing; see the findings."
        };
    return {
        label : "Not validated",
        tone : "warn",
        detail : "Validate it before pipelines, experiments or training can use it."
    };
}

/** Validate's findings, failed ones first. */
export function sortedFindings(findings: unknown): J[] {
    const list: J[] =
        Array.isArray(findings) ? findings.filter((f) => f && typeof f === "object") : [];
    return [...list ].sort((a, b) => Number(a.ok === true) - Number(b.ok === true));
}

export function failedFindings(findings: unknown): number {
    return sortedFindings(findings).filter((f) => f.ok !== true).length;
}

/** A run's phase as a tone: live phases are neutral, the outcome decides the end. */
export function runPhaseTone(run: J): "ok"|"bad"|"warn"|"neutral" {
    const phase = String(run?.phase ?? "");
    if (phase === "Succeeded")
        return run?.outcome === "failed" ? "bad" : "ok";
    if (phase === "Failed")
        return "bad";
    if (phase === "Stopped")
        return "warn";
    return "neutral";
}

/** Whether a run is still under way, so its page refreshes. */
export function runIsLive(run: J): boolean {
    return [ "Pending", "Building", "Queued", "Staging", "Running", "Collecting" ].includes(
        String(run?.phase ?? ""));
}

/**
 * Why a queued run waits, in the operator's words (orama-operator#293): its workspace is at the
 * Lake's cap of concurrent methodology runs, or no node of its GPU class exists yet. Empty for a
 * run that is not queued.
 */
export function runQueuedReason(run: J): string {
    if (String(run?.phase ?? "") !== "Queued")
        return "";
    return String(run?.message ?? "") || "Queued: waiting for a place among this workspace's runs.";
}

/** A run's held-out metrics, the ones to quote first: "held_out_auc 0.84". */
export function heldOutMetrics(run: J): [ string, number ][] {
    const metrics: J =
        run?.result?.metrics && typeof run.result.metrics === "object" ? run.result.metrics : {};
    return Object.entries(metrics)
        .filter(([ name, value ]) => name.startsWith("held_out_") && typeof value === "number")
        .map(([ name, value ]) => [name, value as number]);
}

/** The parameters a workspace methodology declares (the engine keeps them under `interface`). */
export function declaredParameters(entry: J): J[] {
    const list = entry?.interface?.parameters ?? entry?.parameters;
    return Array.isArray(list) ? list.filter((p) => p && typeof p.name === "string") : [];
}

/** Whether the methodology has a target column to name. */
export function needsTarget(entry: J): boolean {
    const target = entry?.interface?.target ?? entry?.target;
    return target?.required === true;
}

/**
 * The parameters as Train sends them: each typed field parsed by its kind, blanks left to the
 * default. Throws with the parameter's name when a value does not parse.
 */
export function parseParams(parameters: J[],
                            values: Record<string, string>): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const parameter of parameters) {
        const name = String(parameter.name);
        const raw = (values[name] ?? "").trim();
        if (!raw)
            continue;
        if (parameter.kind === "int") {
            if (!/^-?\d+$/.test(raw))
                throw new Error(`${name} takes a whole number.`);
            out[name] = Number(raw);
        } else if (parameter.kind === "float") {
            const number = Number(raw);
            if (!Number.isFinite(number))
                throw new Error(`${name} takes a number.`);
            out[name] = number;
        } else if (parameter.kind === "bool") {
            out[name] = raw === "true";
        } else {
            const choices: unknown[] = Array.isArray(parameter.choices) ? parameter.choices : [];
            const choice = choices.find((c) => String(c) === raw);
            if (choice === undefined)
                throw new Error(`${name} takes one of ${choices.map(String).join(", ")}.`);
            out[name] = choice;
        }
    }
    return out;
}

/** The kinds of target a new methodology can draft for, in a person's words. */
export const TARGET_KINDS: {value: string; label : string}[] = [
    {value : "", label : "Let Lake choose from the description"},
    {value : "binary", label : "A yes-or-no outcome"},
    {value : "multiclass", label : "One of several categories"},
    {value : "regression", label : "An amount"},
    {value : "none", label : "No target: find groups"},
];

/** The language a file is highlighted as, by its name. */
export function fileKind(path: string): "python"|"json"|"text" {
    if (path.endsWith(".py"))
        return "python";
    if (path.endsWith(".json"))
        return "json";
    return "text";
}

/** Files in the order a person reads them: the manifest, the entrypoint, the lock, the rest. */
export function orderedFiles(files: unknown, entrypoint = "methodology.py"): J[] {
    const list: J[] =
        Array.isArray(files) ? files.filter((f) => f && typeof f.path === "string") : [];
    const rank = (path: string) => (path === "methodology.json"    ? 0
                                    : path === entrypoint          ? 1
                                    : path === "requirements.lock" ? 2
                                                                   : 3);
    return [...list ].sort((a, b) => rank(a.path) - rank(b.path) ||
                                     String(a.path).localeCompare(String(b.path)));
}

/** One check of a methodology's code, as draft_methodology and Validate answer it. */
export type MethodologyFinding = {check: string; ok: boolean; message: string; file?: string};

/** draft_methodology's draft (lake#688): Ask Orama's, checked by Lake, or Lake's template. */
export type MethodologyDraft = {
    name?: string;
    drafted_by?: "orama"|"template";
    template?: string;
    reason?: string;
    task_id?: string;
    files?: {path: string; contents: string}[];
    findings?: MethodologyFinding[];
    /** Why Ask Orama's draft was not offered, when it was asked for and this is the template. */
    orama?: {task_id?: string; offered?: boolean; reason?: string; findings?: MethodologyFinding[]};
};

/** Who wrote a draft, in the drawer's words: the badge, a note before the reason, and the checks
 * Ask Orama's draft failed when Lake offers its template instead. */
export function draftAuthor(draft: MethodologyDraft, askOramaRunning: boolean): {
    badge: string; tone : "ai" | "neutral" | "warning"; note : string; failing : MethodologyFinding[]
} {
    if (draft.drafted_by === "orama")
        return {badge : "Written by Ask Orama", tone : "ai", note : "", failing : []};
    const failing = (draft.orama?.findings ?? []).filter((finding) => !finding.ok);
    if (draft.orama)
        return {
            badge : "Lake's template",
            tone : "warning",
            note : draft.orama.reason ?? "Ask Orama's draft was not offered.",
            failing
        };
    return {
        badge : "Lake's template",
        tone : "neutral",
        note : askOramaRunning ? "" : "Ask Orama is not running on this Lake, so this is Lake's template.",
        failing : []
    };
}
