// SPDX-License-Identifier: MIT

/** Account → Identity (`/settings?tab=identity`, lake#762): which key the account signs with, and
 * "Use my own identity", which replaces the managed key with the person's own liquid identity in
 * two steps: paste `liquid identity show --document`, then paste what `liquid sign artifact
 * <challenge>` prints (`begin_own_identity`, `use_own_identity`). */
import { useState } from "react";
import { KeyRound } from "lucide-react";
import { act, queryClient, useRead } from "@/lib/api";
import { type AccountIdentity, keySummary, looksSigned, signCommand, signedOutput } from "@/lib/identity";
import { Button, Card, CopyBlock, ErrorBlock, Field, fieldError, Form, KeyValue, Mono, Notice, Skeleton, Status } from "@/lib/ui";
import { NO_WORKSPACE } from "./signIn";

type Begun = { challenge: string; command: string; own_taap_id: string; expires_at: string };

export function IdentityPanel() {
  const identity = useRead<AccountIdentity>("account_identity", undefined, NO_WORKSPACE);
  if (identity.isLoading) return <Skeleton lines={3} />;
  if (identity.isError || !identity.data) return <ErrorBlock error={identity.error} retry={() => void identity.refetch()} what="your identity" />;
  const value = identity.data;
  return (
    <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
      <Card
        className="min-w-0"
        label="Identity"
        title={value.key === "managed" ? "Managed Key" : "Your Own Key"}
        actions={<Status value={value.key === "managed" ? "managed key" : "own key"} tone={value.key === "managed" ? "warning" : "success"} />}
      >
        <p className="mt-0 text-[13px] text-ink-2">{keySummary(value)}</p>
        <KeyValue
          rows={[
            ["Account identity", <Mono key="a">{value.taap_id}</Mono>],
            ["Signs as", <Mono key="s">{value.signs_as}</Mono>],
            ...(value.linked
              ? ([
                  ["Linked", value.linked.linked_at],
                  ["Managed key destroyed", value.managed_key?.destroyed_at ?? "—"],
                ] as [string, string][])
              : []),
          ]}
        />
        <p className="text-[12.5px] text-ink-2">
          Your grants, activity and repositories belong to the account identity, so they stay the same whichever key it signs with.
        </p>
      </Card>
      {value.key === "managed" ? <UseOwnIdentity onDone={() => void queryClient.invalidateQueries({ queryKey: ["account_identity"] })} /> : null}
    </div>
  );
}

function UseOwnIdentity({ onDone }: { onDone: () => void }) {
  const [document, setDocument] = useState("");
  const [begun, setBegun] = useState<Begun | null>(null);
  const [signature, setSignature] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const begin = async () => {
    setBusy(true);
    setError(null);
    try {
      setBegun(await act<Begun>("begin_own_identity", { identity_document: document.trim() }, NO_WORKSPACE));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const finish = async () => {
    if (!begun) return;
    setBusy(true);
    setError(null);
    try {
      await act("use_own_identity", { challenge: begun.challenge, signature: signedOutput(signature) }, NO_WORKSPACE);
      onDone();
    } catch (e) {
      setError(e);
      // A refused signature spends the challenge: start again from the document.
      setBegun(null);
      setSignature("");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card label="Optional" title="Use My Own Identity" className="min-w-0">
      <p className="mt-0 text-[13px] text-ink-2">
        Replace the managed key with your own liquid identity, so pushes you sign with liquid are yours and Lake no longer holds a key for you. Lake destroys the managed key once
        the switch is made. This cannot be undone.
      </p>
      {error ? <Notice tone="danger">{error instanceof Error ? error.message : String(error)}</Notice> : null}
      {!begun ? (
        <Form onSubmit={begin}>
          <p className="m-0 text-[13px]">
            1. Run <code>liquid identity show --document</code> and paste its output.
          </p>
          <Field label="Your identity document" error={fieldError(error, "identity_document")} required>
            <textarea name="identity_document" spellCheck={false} value={document} onChange={(e) => setDocument(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" variant="primary" loading={busy} disabled={!document.trim()}>
              <KeyRound size={14} aria-hidden /> Continue
            </Button>
          </div>
        </Form>
      ) : (
        <Form onSubmit={finish}>
          <p className="m-0 text-[13px]">
            2. Sign this challenge with your identity before {begun.expires_at}, and paste everything the command prints.
          </p>
          <div className="min-w-0 [&_pre]:break-all [&_pre]:whitespace-pre-wrap">
            <CopyBlock label="Run" value={signCommand(begun.challenge)} multiline />
          </div>
          <Field label="What liquid sign printed" error={fieldError(error, "signature")} required>
            <textarea name="signature" spellCheck={false} value={signature} onChange={(e) => setSignature(e.target.value)} />
          </Field>
          <div className="flex gap-2">
            <Button type="submit" variant="primary" loading={busy} disabled={!looksSigned(signature)}>
              Use my own identity
            </Button>
            <Button onClick={() => setBegun(null)}>Start again</Button>
          </div>
        </Form>
      )}
    </Card>
  );
}
