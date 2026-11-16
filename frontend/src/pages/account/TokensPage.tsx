// SPDX-License-Identifier: MIT

/** API tokens (`/tokens`), which Developer → API and MCP call keys: create a named, scoped token in a drawer (its secret shown once, in this
 * page's memory only) and revoke existing ones. The token rules (attenuation, never-expiring
 * tokens) are the capability's; the page only builds its input as the form handler did. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { KeyRound, Plus } from "lucide-react";
import { act, useList } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Chip, CopyBlock, DataTable, Field, fieldError, FilterChips, Form, Notice, Page, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { NameCell, PanelEmpty, When, matches } from "@/pages/organizations/shared";
import { ActiveBadge, type SessionRow, sessionKind } from "./labels";
import { messageOf, NO_WORKSPACE } from "./signIn";
import { LIQUID_SCOPES } from "@/pages/developer/logic";

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
  const [creating, setCreating] = useState(false);
  const [secret, setSecret] = useState<string | null>(null);
  const [revokeError, setRevokeError] = useState<string | null>(null);
  const [revoking, setRevoking] = useState<string | null>(null);
  const [search, setSearch] = useState("");
  const [state, setState] = useState<"active" | "revoked" | null>(null);

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
  const rows = tokens?.filter((row) => matches(search, row.name, ...(row.scopes ?? [])) && (!state || (state === "revoked") === !!row.revoked_at));
  const active = (tokens ?? []).filter((row) => !row.revoked_at);

  return (
    <Page
      eyebrow="Account"
      title="API Tokens"
      sub={
        <>
          Create named, scoped tokens and revoke them here. The keys Developer → <Link to="/developer/api">API</Link> and <Link to="/developer/mcp">MCP</Link> make are API
          tokens too, and are listed here.
        </>
      }
      actions={
        <Button variant="primary" onClick={() => setCreating(true)}>
          <Plus size={14} aria-hidden /> New token
        </Button>
      }
    >
      {secret && (
        <Banner tone="ok" title="Copy this token now. It is not shown again." action={<Button onClick={() => setSecret(null)}>Done</Button>}>
          <div className="mt-2">
            <CopyBlock value={secret} label="Token" />
          </div>
        </Banner>
      )}
      {revokeError && <Notice tone="danger">{revokeError}</Notice>}
      {tokens && tokens.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Tokens" value={tokens.length} icon={KeyRound} />
          <Stat label="Active" value={active.length} />
          <Stat label="Never expire" value={active.filter((row) => !row.expires_at).length} sub="no expiry" />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search tokens…" />
        <FilterChips
          options={[
            { id: "active" as const, label: "Active", count: active.length },
            { id: "revoked" as const, label: "Revoked", count: (tokens?.length ?? 0) - active.length },
          ]}
          value={state}
          onChange={setState}
          allLabel="Any state"
        />
      </Toolbar>
      <DataTable<SessionRow>
        q={sessions}
        what="tokens"
        rows={rows}
        rowKey={(row) => row.id}
        empty={
          <PanelEmpty
            title="No API tokens"
            action={
              tokens?.length ? undefined : (
                <Button variant="primary" onClick={() => setCreating(true)}>
                  New token
                </Button>
              )
            }
          >
            {tokens?.length ? "Clear the search or the filter to see more." : "A token lets a script, an agent or Liquid act with your permissions, limited to the scopes you name."}
          </PanelEmpty>
        }
        columns={[
          { key: "name", label: "Token", sort: (row) => row.name, render: (row) => <NameCell name={row.name || "—"} sub={sessionKind(row.kind)} /> },
          {
            key: "scopes",
            label: "Scopes",
            render: (row) =>
              (row.scopes ?? []).length ? (
                <span className="flex max-w-[24rem] flex-wrap gap-1">
                  {(row.scopes ?? []).map((s) => (
                    <Chip key={s}>{s}</Chip>
                  ))}
                </span>
              ) : (
                <span className="muted">—</span>
              ),
          },
          {
            key: "expires",
            label: "Expires",
            sort: (row) => row.expires_at ?? "",
            render: (row) =>
              row.expires_at ? (
                <span title={dateTime(row.expires_at)}>
                  <When value={row.expires_at} />
                </span>
              ) : (
                <>
                  Never expires
                  {row.reason && (
                    <>
                      {" "}
                      <small className="block text-ink-3">{row.reason}</small>
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
      {creating && (
        <NewToken
          onClose={() => setCreating(false)}
          onCreated={(value) => {
            setSecret(value);
            setCreating(false);
          }}
        />
      )}
    </Page>
  );
}

function NewToken({ onClose, onCreated }: { onClose: () => void; onCreated: (secret: string | null) => void }) {
  const [name, setName] = useState("");
  const [scopes, setScopes] = useState("");
  const [expiresIn, setExpiresIn] = useState("");
  const [isStatic, setStatic] = useState(false);
  const [neverExpires, setNeverExpires] = useState(false);
  const [reason, setReason] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const create = async () => {
    setBusy(true);
    setError(null);
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
      onCreated(value.secret ?? null);
      window.scrollTo({ top: 0 });
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const placed = ["name", "scopes", "expires_in_seconds", "reason"].some((field) => fieldError(error, field));
  return (
    <Drawer
      title="New Token"
      subtitle="Its secret is shown once, after it is created."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="create-token" variant="primary" loading={busy}>
            Create token
          </Button>
        </>
      }
    >
      {error && !placed ? <Notice tone="danger">{messageOf(error)}</Notice> : null}
      <Form id="create-token" onSubmit={create}>
        <Field label="Name" error={fieldError(error, "name")} required>
          <input type="text" name="name" value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
        <Field
          label="Capability scopes, comma separated"
          error={fieldError(error, "scopes")}
          required
          help={
            <>
              For liquid and liquify (clone, push, pull and reviews), use <code>{LIQUID_SCOPES}</code>{" "}
              <button type="button" className="button-ghost inline p-0 text-action underline" onClick={() => setScopes(LIQUID_SCOPES)}>
                Use these
              </button>
              . The token keeps your existing repository permissions.
            </>
          }
        >
          <input type="text" name="scopes" value={scopes} onChange={(e) => setScopes(e.target.value)} />
        </Field>
        <Field label="Expiry in seconds" error={fieldError(error, "expires_in_seconds")} help="Leave empty for as long as this session allows, at most 30 days.">
          <input type="number" name="expires_in_seconds" min={1} value={expiresIn} onChange={(e) => setExpiresIn(e.target.value)} />
        </Field>
        <label className="flex items-start gap-2 text-[13px]">
          <input type="checkbox" name="static" checked={isStatic} onChange={(e) => setStatic(e.target.checked)} />
          <span>Static exception</span>
        </label>
        <label className="flex items-start gap-2 text-[13px]">
          <input type="checkbox" name="never_expires" checked={neverExpires} onChange={(e) => setNeverExpires(e.target.checked)} />
          <span>Never expires (no expiry; needs your workspace's opt-in, a reason and named scopes)</span>
        </label>
        <Field label="Reason for a static or never-expiring token" error={fieldError(error, "reason")}>
          <input type="text" name="reason" value={reason} onChange={(e) => setReason(e.target.value)} />
        </Field>
      </Form>
    </Drawer>
  );
}
