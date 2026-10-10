/** API tokens (`/tokens`): create a named, scoped token (its secret shown once, in this page's
 * memory only) and revoke existing ones. The token rules (attenuation, never-expiring tokens) are
 * the capability's; the page only builds its input as the form handler did. */
import { useState } from "react";
import { act, useList } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, CopyBlock, DataTable, Field, fieldError, Form, Notice, Page, Section } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { ActiveBadge, type SessionRow, sessionKind } from "./labels";
import { messageOf, NO_WORKSPACE } from "./signIn";

/** "a, b,,c " → ["a", "b", "c"]: trimmed, empties dropped. */
export function commaSeparated(value: string): string[] {
  return value
    .split(",")
    .map((part) => part.trim())
    .filter(Boolean);
}

export default function TokensPage() {
  useTrail([{ label: "Account", to: "/settings" }, { label: "Tokens" }]);
  const toast = useToast();
  const sessions = useList<SessionRow>("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const tokens = sessions.data?.filter((row) => row.kind === "api_token" || row.kind === "static");

  const [name, setName] = useState("");
  const [scopes, setScopes] = useState("");
  const [expiresIn, setExpiresIn] = useState("");
  const [isStatic, setStatic] = useState(false);
  const [neverExpires, setNeverExpires] = useState(false);
  const [reason, setReason] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [secret, setSecret] = useState<string | null>(null);
  const [revokeError, setRevokeError] = useState<string | null>(null);
  const [revoking, setRevoking] = useState<string | null>(null);

  const create = async () => {
    setBusy(true);
    setError(null);
    setSecret(null);
    try {
      const seconds = expiresIn.trim() ? Number.parseInt(expiresIn, 10) : null;
      const value = await act<{ secret?: string }>(
        "create_token",
        {
          name,
          scopes: commaSeparated(scopes),
          static: isStatic,
          ...(reason.trim() ? { reason: reason.trim() } : {}),
          ...(neverExpires ? { never_expires: true } : {}),
          ...(seconds !== null && Number.isFinite(seconds) ? { expires_in_seconds: seconds } : {}),
        },
        NO_WORKSPACE,
      );
      setSecret(value.secret ?? null);
      setName("");
      setScopes("");
      setExpiresIn("");
      setStatic(false);
      setNeverExpires(false);
      setReason("");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const revoke = async (row: SessionRow) => {
    setRevoking(row.id);
    setRevokeError(null);
    try {
      await act("revoke_session", { id: row.id }, NO_WORKSPACE);
      toast("Token revoked");
    } catch (e) {
      setRevokeError(messageOf(e));
    } finally {
      setRevoking(null);
    }
  };

  return (
    <Page eyebrow="identity" title="API Tokens" sub="Create named, scoped sessions and revoke them here.">
      <p>
        For Liquid push and pull, use these capability scopes: <code>refs,find_missing,push,pull,upload_chunks,read_chunk</code>. The token keeps your existing
        repository permissions.
      </p>
      {error ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      {revokeError && <Notice tone="danger">{revokeError}</Notice>}
      {secret && (
        <div className="notice" role="status">
          <p className="m-0">Copy this token now. It is not shown again.</p>
          <CopyBlock value={secret} label="Token" />
        </div>
      )}
      <div className="panel">
        <Form onSubmit={create}>
          <Field label="Name" error={fieldError(error, "name")} required>
            <input type="text" name="name" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Capability scopes, comma separated" error={fieldError(error, "scopes")} required>
            <input type="text" name="scopes" value={scopes} onChange={(e) => setScopes(e.target.value)} />
          </Field>
          <Field label="Expiry in seconds (leave empty for as long as this session allows, at most 30 days)" error={fieldError(error, "expires_in_seconds")}>
            <input type="number" name="expires_in_seconds" min={1} value={expiresIn} onChange={(e) => setExpiresIn(e.target.value)} />
          </Field>
          <label>
            <span>
              <input type="checkbox" name="static" checked={isStatic} onChange={(e) => setStatic(e.target.checked)} /> Static exception
            </span>
          </label>
          <label>
            <span>
              <input type="checkbox" name="never_expires" checked={neverExpires} onChange={(e) => setNeverExpires(e.target.checked)} /> Never expires (survives
              sign-out; needs your space's opt-in, a reason and named scopes)
            </span>
          </label>
          <Field label="Reason for a static or never-expiring token" error={fieldError(error, "reason")}>
            <input type="text" name="reason" value={reason} onChange={(e) => setReason(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" variant="primary" loading={busy}>
              Create token
            </Button>
          </div>
        </Form>
      </div>
      <Section title="Existing Tokens">
        <DataTable<SessionRow>
          q={sessions}
          what="tokens"
          rows={tokens}
          rowKey={(row) => row.id}
          empty="No API tokens."
          columns={[
            { key: "name", label: "Name", render: (row) => row.name || "—" },
            { key: "kind", label: "Kind", render: (row) => sessionKind(row.kind) },
            { key: "scopes", label: "Scopes", render: (row) => (row.scopes ?? []).join(", ") || "—" },
            {
              key: "expires",
              label: "Expires",
              render: (row) =>
                row.expires_at ? (
                  dateTime(row.expires_at)
                ) : (
                  <>
                    Never expires
                    {row.reason && (
                      <>
                        {" "}
                        <small>{row.reason}</small>
                      </>
                    )}
                  </>
                ),
            },
            { key: "status", label: "Status", render: (row) => <ActiveBadge revokedAt={row.revoked_at} /> },
            {
              key: "revoke",
              label: <span className="sr-only">Actions</span>,
              render: (row) => (
                <Button onClick={() => void revoke(row)} loading={revoking === row.id} disabled={!!row.revoked_at}>
                  Revoke
                </Button>
              ),
            },
          ]}
        />
      </Section>
    </Page>
  );
}
