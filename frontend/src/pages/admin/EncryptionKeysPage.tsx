/** Encryption Keys (`/encryption-keys?mount_id=`): a mount's named, versioned keys, never their
 * material (web_ui_encryption_keys, serve_encryption_keys_page), with New Key
 * (`create_encryption_key`, or `derive_encryption_key` from a parent), Rotate
 * (`rotate_encryption_key`) and a Try It drawer for encrypt, decrypt, rewrap, sign, verify and HMAC
 * (docs/ui/secrets.md). What is typed into Try It stays in the drawer's state. */
import { useState } from "react";
import { Navigate, useNavigate } from "react-router-dom";
import { FlaskConical, GitBranch, KeyRound, Lock, Plus, RefreshCw } from "lucide-react";
import { act, useList, useRead, type Row } from "@/lib/api";
import { ConfirmButton, Drawer, Modal, SheetActions } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, CopyBlock, DataTable, Empty, ErrorBlock, Field, Form, Mono, Page, SearchInput, Skeleton, StateTabs, Stat, StatRow, Status, Toolbar, fieldError } from "@/lib/ui";
import { HANDLE_PATTERN, NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { fromBase64Url, toBase64Url } from "./secretValue";

export default function EncryptionKeysPage() {
  const mountId = useParam("mount_id");
  const mount = useRead<Row>(mountId ? "get_secret_mount" : null, { mount_id: mountId });
  const keys = useList<Row>(mountId ? "list_encryption_keys" : null, { mount_id: mountId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  const [creating, setCreating] = useState(false);
  const [trying, setTrying] = useState<Row | null>(null);
  useMetaTrail(keys.data ? `${keys.data.length} keys` : null, "Encryption Keys");
  if (!mountId) return <Navigate to="/secret-mounts" replace />;
  if (mount.isError) {
    return (
      <Page eyebrow="Organization" title="Encryption Keys">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="Organization" title="Encryption Keys">
        <Skeleton lines={5} />
      </Page>
    );
  }
  const rows = keys.data?.filter((k) => matches(search, k.name, k.derived_from));
  const newKey = (
    <Button variant="primary" onClick={() => setCreating(true)}>
      <Plus size={14} aria-hidden /> New key
    </Button>
  );
  return (
    <Page
      eyebrow="Organization"
      title={`${mount.data.name} Keys`}
      sub="Named, versioned keys for encryption, signing, HMAC and derivation. Key material is never returned."
      actions={
        <>
          <Button to={`/secrets?mount_id=${encodeURIComponent(mountId)}`}>
            <Lock size={14} aria-hidden /> Static secrets
          </Button>
          {newKey}
        </>
      }
    >
      {keys.data && keys.data.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Keys" value={keys.data.length} icon={KeyRound} />
          <Stat label="Rotated" value={keys.data.filter((k) => Number(k.current_version) > 1).length} sub="past their first version" />
          <Stat label="Derived" value={keys.data.filter((k) => k.derived_from).length} sub="children of another key" />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search keys…" />
      </Toolbar>
      <DataTable
        q={keys}
        what="encryption keys"
        rows={rows}
        rowKey={(k) => k.id}
        empty={
          keys.data?.length ? (
            <PanelEmpty title="No key matches">Clear the search to see every key.</PanelEmpty>
          ) : (
            <Empty icon={KeyRound} title="No keys yet" action={newKey}>
              A key encrypts, signs and computes HMACs inside Lake; its material never leaves.
            </Empty>
          )
        }
        columns={[
          { key: "name", label: "Key", sort: (k) => k.name, render: (k) => <NameCell name={<code>{k.name}</code>} sub={k.derived_from ? <>derived from <Mono short={20}>{k.derived_from}</Mono></> : undefined} /> },
          { key: "version", label: "Current version", numeric: true, sort: (k) => Number(k.current_version), render: (k) => <span className="font-mono">{k.current_version}</span> },
          { key: "created", label: "Created", sort: (k) => k.created_at, render: (k) => <When value={k.created_at} /> },
          { key: "updated", label: "Updated", sort: (k) => k.updated_at, render: (k) => <When value={k.updated_at} /> },
          {
            key: "actions",
            label: <span className="sr-only">Actions</span>,
            render: (k) => (
              <span className="flex flex-wrap justify-end gap-1.5">
                <Button variant="ghost" onClick={() => setTrying(k)}>
                  <FlaskConical size={14} aria-hidden /> Try it
                </Button>
                <RotateKey mountId={mountId} keyRow={k} />
              </span>
            ),
          },
        ]}
      />
      {creating && <NewKey mountId={mountId} keys={keys.data ?? []} onClose={() => setCreating(false)} />}
      {trying && <TryKey mountId={mountId} keyRow={trying} onClose={() => setTrying(null)} />}
    </Page>
  );
}

function RotateKey({ mountId, keyRow }: { mountId: string; keyRow: Row }) {
  const toast = useToast();
  const next = Number(keyRow.current_version) + 1;
  return (
    <ConfirmButton
      variant="ghost"
      title={`Rotate ${keyRow.name}?`}
      confirmLabel="Rotate key"
      confirm={
        <>
          Version {next} of <code>{keyRow.name}</code> becomes current: encrypt, sign and HMAC use it from now on. Older versions still decrypt and verify, and rewrap moves old ciphertext to version {next}.
        </>
      }
      onConfirm={async () => {
        const rotated = await act<Row>("rotate_encryption_key", { mount_id: mountId, name: keyRow.name });
        toast(`${rotated.name} is now at version ${rotated.current_version}`);
      }}
    >
      <RefreshCw size={14} aria-hidden /> Rotate
    </ConfirmButton>
  );
}

/** New Key: a fresh key, or one derived from a parent key and a context. */
function NewKey({ mountId, keys, onClose }: { mountId: string; keys: Row[]; onClose: () => void }) {
  const toast = useToast();
  const navigate = useNavigate();
  const [name, setName] = useState("");
  const [parent, setParent] = useState("");
  const [context, setContext] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const created = parent
        ? await act<Row>("derive_encryption_key", { mount_id: mountId, parent_name: parent, name, context: toBase64Url(context) })
        : await act<Row>("create_encryption_key", { mount_id: mountId, name });
      toast(parent ? `Derived ${created.name} from ${parent}` : `Created key ${created.name}`);
      onClose();
      navigate(`/encryption-keys?mount_id=${encodeURIComponent(mountId)}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const fieldFault = ["name", "parent_name", "context"].some((f) => fieldError(error, f));
  return (
    <Modal title="New Encryption Key" subtitle="One key encrypts, signs (Ed25519) and computes HMAC-SHA-256; its material stays in Lake." onClose={onClose} sheet>
      <Form onSubmit={submit} error={fieldFault ? null : error}>
        <Field label="Name" required help="Lowercase letters, digits and dashes; unique in this mount." error={fieldError(error, "name")}>
          <input value={name} onChange={(e) => setName(e.target.value)} pattern={HANDLE_PATTERN} placeholder="customer-data" autoComplete="off" spellCheck={false} />
        </Field>
        {keys.length > 0 && (
          <Field label="Derive from" help="Optional: store a child of another key, derived from it and a context." error={fieldError(error, "parent_name")}>
            <select value={parent} onChange={(e) => setParent(e.target.value)}>
              <option value="">Nothing: a new, independent key</option>
              {keys.map((k) => (
                <option key={k.id} value={k.name}>
                  {k.name}
                </option>
              ))}
            </select>
          </Field>
        )}
        {parent && (
          <Field label="Context" required help="Text that makes this child distinct, such as a region or tenant; sent as base64url." error={fieldError(error, "context")}>
            <input value={context} onChange={(e) => setContext(e.target.value)} placeholder="eu" autoComplete="off" spellCheck={false} />
          </Field>
        )}
        <SheetActions className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" variant="primary" loading={busy} disabled={!name || (!!parent && !context)}>
            {parent ? <GitBranch size={14} aria-hidden /> : null} {parent ? "Derive key" : "Create key"}
          </Button>
        </SheetActions>
      </Form>
    </Modal>
  );
}

type Operation = "encrypt" | "decrypt" | "rewrap" | "sign" | "verify" | "hmac";

const OPERATIONS: { value: Operation; label: string; help: string }[] = [
  { value: "encrypt", label: "Encrypt", help: "Text → ciphertext under the current version." },
  { value: "decrypt", label: "Decrypt", help: "Ciphertext from this key → the text." },
  { value: "rewrap", label: "Rewrap", help: "Old ciphertext → ciphertext under the current version; the text never leaves Lake." },
  { value: "sign", label: "Sign", help: "Text → an Ed25519 signature." },
  { value: "verify", label: "Verify", help: "Text and a signature → valid or not." },
  { value: "hmac", label: "HMAC", help: "Text → an HMAC-SHA-256." },
];

/** Try It: one operation at a time against this key. Text is sent as base64url, as the API takes it. */
function TryKey({ mountId, keyRow, onClose }: { mountId: string; keyRow: Row; onClose: () => void }) {
  const [operation, setOperation] = useState<Operation>("encrypt");
  const [text, setText] = useState("");
  const [ciphertext, setCiphertext] = useState("");
  const [signature, setSignature] = useState("");
  const [version, setVersion] = useState("");
  const [result, setResult] = useState<{ operation: Operation; value?: string; text?: string | null; valid?: boolean; version?: number; algorithm?: string } | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const takesText = operation === "encrypt" || operation === "sign" || operation === "verify" || operation === "hmac";
  const takesCiphertext = operation === "decrypt" || operation === "rewrap";
  const takesVersion = operation === "sign" || operation === "verify" || operation === "hmac";
  const choose = (next: Operation) => {
    setOperation(next);
    setResult(null);
    setError(null);
  };
  const run = async () => {
    setBusy(true);
    setError(null);
    setResult(null);
    try {
      const input: Row = { mount_id: mountId, name: keyRow.name };
      if (takesText) input[operation === "encrypt" ? "plaintext" : "message"] = toBase64Url(text);
      if (takesCiphertext) input.ciphertext = ciphertext.trim();
      if (operation === "verify") input.signature = signature.trim();
      if (takesVersion && version.trim()) input.version = Number(version);
      const answer = await act<Row>(operation, input);
      setResult({
        operation,
        value: answer.value,
        text: operation === "decrypt" ? fromBase64Url(answer.value ?? "") : undefined,
        valid: answer.valid,
        version: answer.key_version,
        algorithm: answer.algorithm,
      });
      // The next step reads what this one wrote: a ciphertext to decrypt, a signature to verify.
      if (operation === "encrypt" || operation === "rewrap") setCiphertext(answer.value ?? "");
      if (operation === "sign") setSignature(answer.value ?? "");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const help = OPERATIONS.find((o) => o.value === operation)?.help;
  return (
    <Drawer title="Try It" subtitle={<code>{keyRow.name}</code>} onClose={onClose} width={560}>
      <StateTabs label="Operation" value={operation} onChange={choose} tabs={OPERATIONS.map((o) => ({ value: o.value, label: o.label }))} />
      <Banner tone="quiet">{help} Every call is audited; what you type here is not kept.</Banner>
      <Form onSubmit={run} error={error}>
        {takesText && (
          <Field label={operation === "encrypt" ? "Plaintext" : "Message"} required>
            <textarea rows={3} value={text} onChange={(e) => setText(e.target.value)} spellCheck={false} autoComplete="off" />
          </Field>
        )}
        {takesCiphertext && (
          <Field label="Ciphertext" required help="lake:v1:<key id>:<version>:<sealed>, as encrypt answers it.">
            <textarea rows={3} className="font-mono" value={ciphertext} onChange={(e) => setCiphertext(e.target.value)} spellCheck={false} autoComplete="off" />
          </Field>
        )}
        {operation === "verify" && (
          <Field label="Signature" required help="base64url, as sign answers it.">
            <input className="font-mono" value={signature} onChange={(e) => setSignature(e.target.value)} spellCheck={false} autoComplete="off" />
          </Field>
        )}
        {takesVersion && (
          <Field label="Key version" help={`Optional: the current version (${keyRow.current_version}) when empty.`}>
            <input type="number" min={1} step={1} value={version} onChange={(e) => setVersion(e.target.value)} />
          </Field>
        )}
        <SheetActions className="flex justify-end gap-2">
          <Button type="submit" variant="primary" loading={busy} disabled={(takesText && !text) || (takesCiphertext && !ciphertext.trim()) || (operation === "verify" && !signature.trim())}>
            {OPERATIONS.find((o) => o.value === operation)?.label}
          </Button>
        </SheetActions>
      </Form>
      {result && (
        <div className="mt-4 space-y-2">
          {result.operation === "verify" ? (
            <p className="m-0 flex items-center gap-2">
              <Status value={result.valid ? "valid" : "invalid"} tone={result.valid ? "success" : "danger"} /> checked with version {result.version}
            </p>
          ) : result.operation === "decrypt" ? (
            result.text !== null ? (
              <CopyBlock value={result.text ?? ""} label={`Plaintext (version ${result.version})`} multiline />
            ) : (
              <CopyBlock value={result.value ?? ""} label={`Plaintext as base64url (version ${result.version}; not UTF-8 text)`} multiline />
            )
          ) : (
            <CopyBlock value={result.value ?? ""} label={`${OPERATIONS.find((o) => o.value === result.operation)?.label} (version ${result.version}${result.algorithm ? `, ${result.algorithm}` : ""})`} multiline />
          )}
        </div>
      )}
    </Drawer>
  );
}
