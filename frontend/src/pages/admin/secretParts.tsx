// SPDX-License-Identifier: MIT

/** What the Secrets pages share (docs/ui/secrets.md): the value editor a new secret and a new
 * version use, the masked value a deliberate Reveal shows, and the drawer that writes a version.
 * A value lives only in component state: never in the URL, a log, the query cache or storage, and
 * it is gone when the drawer closes or the page is left. */
import { useState } from "react";
import { Check, Copy, Eye, EyeOff, Plus, Trash2 } from "lucide-react";
import { ApiError, act, type Row } from "@/lib/api";
import { Drawer, SheetActions } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Field, Form, StateTabs, cx, fieldError } from "@/lib/ui";
import { fieldText, parseJsonObject, rowsToValue, valueToRows, type Parsed, type ValueRow } from "./secretValue";

type Mode = "rows" | "json";

/** The value as key/value rows or as raw JSON; switching carries what is written across when it can. */
export function ValueEditor({
  mode,
  onMode,
  rows,
  onRows,
  json,
  onJson,
  error,
  badRow,
}: {
  mode: Mode;
  onMode: (mode: Mode) => void;
  rows: ValueRow[];
  onRows: (rows: ValueRow[]) => void;
  json: string;
  onJson: (json: string) => void;
  error?: string;
  badRow?: number;
}) {
  const [note, setNote] = useState<string | null>(null);
  const switchTo = (next: Mode) => {
    setNote(null);
    if (next === mode) return;
    if (next === "json") {
      const parsed = rowsToValue(rows);
      onJson(parsed.ok ? JSON.stringify(parsed.value, null, 2) : json || "{\n  \n}");
    } else {
      const parsed = parseJsonObject(json);
      const asRows = parsed.ok ? valueToRows(parsed.value) : null;
      if (parsed.ok && !asRows) {
        setNote("Some fields are not text (numbers, objects), so they stay in the JSON editor.");
        return;
      }
      if (asRows) onRows(asRows.length ? asRows : [{ key: "", value: "" }]);
    }
    onMode(next);
  };
  const setRow = (index: number, patch: Partial<ValueRow>) => onRows(rows.map((row, i) => (i === index ? { ...row, ...patch } : row)));
  return (
    <div className="field">
      <span className="field-label">Value</span>
      <StateTabs
        label="Value format"
        value={mode}
        onChange={switchTo}
        tabs={[
          { value: "rows", label: "Key and value" },
          { value: "json", label: "JSON" },
        ]}
      />
      {note && <small className="muted">{note}</small>}
      {mode === "rows" ? (
        <div className="mt-2 space-y-2">
          {rows.map((row, index) => (
            <div key={index} className={cx("flex flex-wrap items-center gap-2", badRow === index && "rounded-[6px] outline outline-1 outline-bad")}>
              <input
                aria-label={`Key ${index + 1}`}
                className="min-w-0 flex-1 basis-[8rem] font-mono"
                placeholder="key"
                value={row.key}
                onChange={(e) => setRow(index, { key: e.target.value })}
                autoComplete="off"
                spellCheck={false}
              />
              <input
                aria-label={`Value ${index + 1}`}
                className="min-w-0 flex-[2] basis-[10rem] font-mono"
                type="password"
                placeholder="value"
                value={row.value}
                onChange={(e) => setRow(index, { value: e.target.value })}
                autoComplete="new-password"
                spellCheck={false}
              />
              <button
                type="button"
                className="icon-button"
                aria-label={`Remove row ${index + 1}`}
                onClick={() => onRows(rows.length > 1 ? rows.filter((_, i) => i !== index) : [{ key: "", value: "" }])}
              >
                <Trash2 size={14} aria-hidden />
              </button>
            </div>
          ))}
          <Button onClick={() => onRows([...rows, { key: "", value: "" }])}>
            <Plus size={14} aria-hidden /> Add a field
          </Button>
        </div>
      ) : (
        <textarea
          aria-label="Value as JSON"
          className="mt-2 font-mono"
          rows={8}
          value={json}
          onChange={(e) => onJson(e.target.value)}
          spellCheck={false}
          autoComplete="off"
          placeholder={'{\n  "password": "…"\n}'}
        />
      )}
      {error ? (
        <small className="field-error" role="alert">
          {error}
        </small>
      ) : (
        <small className="muted">One JSON object. Values are written once and never shown here again.</small>
      )}
    </div>
  );
}

/** The value a form holds, checked: rows or JSON → the object `put_secret` takes. */
function valueOf(mode: Mode, rows: ValueRow[], json: string): Parsed {
  return mode === "rows" ? rowsToValue(rows) : parseJsonObject(json);
}

/** Write a secret: a new one (`name` empty: the path is asked for) or a new version of `name`.
 * `currentVersion` makes the write check-and-set, so a version written by someone else meanwhile
 * is a conflict, not an overwrite. */
export function WriteSecretDrawer({
  mountId,
  name: fixedName,
  currentVersion,
  existing,
  onClose,
  onWritten,
}: {
  mountId: string;
  name?: string;
  currentVersion?: number;
  /** The secrets already listed, by path, with their current version: a new secret whose path is
   * taken becomes that secret's next version, checked against it. */
  existing?: Map<string, number>;
  onClose: () => void;
  onWritten: (written: Row) => void;
}) {
  const toast = useToast();
  const [path, setPath] = useState("");
  const [mode, setMode] = useState<Mode>("rows");
  const [rows, setRows] = useState<ValueRow[]>([
    { key: "", value: "" },
  ]);
  const [json, setJson] = useState("");
  const [metadata, setMetadata] = useState("");
  const [problem, setProblem] = useState<{ field: "value" | "metadata"; message: string; row?: number } | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const name = fixedName ?? path.trim();
  const taken = !fixedName && name ? existing?.get(name) : undefined;
  const expected = fixedName ? currentVersion : taken;
  const submit = async () => {
    setProblem(null);
    setError(null);
    const value = valueOf(mode, rows, json);
    if (!value.ok) {
      setProblem({ field: "value", message: value.message, row: value.row });
      return;
    }
    const input: Row = { mount_id: mountId, name, value: value.value };
    if (metadata.trim()) {
      const parsed = parseJsonObject(metadata, "Custom metadata");
      if (!parsed.ok) {
        setProblem({ field: "metadata", message: parsed.message });
        return;
      }
      input.custom_metadata = parsed.value;
    }
    if (expected) input.check_and_set = expected;
    setBusy(true);
    try {
      const written = await act<Row>("put_secret", input);
      toast(`Saved ${written.name} as version ${written.version}`);
      onWritten(written);
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const conflict = error instanceof ApiError && error.path === "/check_and_set";
  const fieldFault = !!(fieldError(error, "name") || fieldError(error, "value") || fieldError(error, "custom_metadata"));
  return (
    <Drawer
      title={fixedName ? "New Version" : "New Secret"}
      subtitle={fixedName ? <code>{fixedName}</code> : "A path and a JSON object, written as version 1."}
      onClose={onClose}
      width={560}
    >
      <Form onSubmit={submit} error={conflict || fieldFault ? null : error}>
        {conflict && (
          <Banner tone="warn" title="Someone wrote a newer version">
            Version {expected} is no longer current, so nothing was written. Close this, look at the new version, and write again.
          </Banner>
        )}
        {!fixedName && (
          <Field label="Path" required help="Slashes group secrets: database/password, payments/stripe." error={fieldError(error, "name")}>
            <input value={path} onChange={(e) => setPath(e.target.value)} placeholder="database/password" autoComplete="off" spellCheck={false} maxLength={1024} />
          </Field>
        )}
        {taken !== undefined && (
          <Banner tone="info">
            <code>{name}</code> already exists. Saving adds version {taken + 1}, checked against version {taken}.
          </Banner>
        )}
        <ValueEditor
          mode={mode}
          onMode={setMode}
          rows={rows}
          onRows={setRows}
          json={json}
          onJson={setJson}
          error={problem?.field === "value" ? problem.message : fieldError(error, "value")}
          badRow={problem?.field === "value" ? problem.row : undefined}
        />
        <details className="text-[13px]">
          <summary className="cursor-pointer text-ink-2">Custom metadata</summary>
          <Field
            label="Custom metadata (JSON)"
            help={fixedName ? "Leave empty to keep the secret's metadata; an object replaces it." : "Optional: an object kept with the secret, such as an owner."}
            error={problem?.field === "metadata" ? problem.message : fieldError(error, "custom_metadata")}
          >
            <textarea rows={4} className="font-mono" value={metadata} onChange={(e) => setMetadata(e.target.value)} placeholder='{"owner": "payments"}' spellCheck={false} />
          </Field>
        </details>
        <SheetActions className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" variant="primary" loading={busy} disabled={!name}>
            {fixedName ? "Save version" : "Save secret"}
          </Button>
        </SheetActions>
      </Form>
    </Drawer>
  );
}

/** A revealed value: each field masked until shown, each with Copy, and Copy for the whole object. */
export function RevealedValue({ value }: { value: Record<string, unknown> }) {
  const [shown, setShown] = useState(false);
  const entries = Object.entries(value);
  return (
    <div>
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <Button onClick={() => setShown((s) => !s)}>
          {shown ? <EyeOff size={14} aria-hidden /> : <Eye size={14} aria-hidden />} {shown ? "Mask" : "Show"}
        </Button>
        <CopyButton text={JSON.stringify(value, null, 2)} label="Copy JSON" />
      </div>
      {entries.length === 0 ? (
        <p className="muted m-0">An empty object.</p>
      ) : (
        <div className="divide-y divide-hairline rounded-[6px] border border-hairline" role="list" aria-label="Fields">
          {entries.map(([key, field]) => (
            <div key={key} role="listitem" className="flex flex-wrap items-center gap-x-3 gap-y-1 px-3 py-2">
              <span className="min-w-0 basis-[9rem] font-mono text-[12.5px] text-ink-2">{key}</span>
              <span className="min-w-0 flex-1 break-all font-mono text-[13px] text-ink">{shown ? fieldText(field) : "••••••••••••"}</span>
              <CopyButton text={fieldText(field)} label={`Copy ${key}`} compact />
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

function CopyButton({ text, label, compact }: { text: string; label: string; compact?: boolean }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {
      /* Show the value and select it instead. */
    }
  };
  const icon = copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />;
  return compact ? (
    <button type="button" className="icon-button" onClick={copy} aria-label={copied ? "Copied" : label} title={label}>
      {icon}
    </button>
  ) : (
    <Button onClick={copy} aria-label={copied ? "Copied" : label}>
      {icon} {copied ? "Copied" : label}
    </Button>
  );
}
