// SPDX-License-Identifier: MIT

/** Member settings (`/member?id=&confirm_removal=yes`): one membership, its role and its removal
 * behind a confirmation (web_ui_member, serve_member_page, serve_member_role,
 * serve_member_remove). `confirm_removal=yes` opens the confirmation at once. */
import { useEffect, useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { UserMinus } from "lucide-react";
import { act, useRead, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, Chip, DataTable, ErrorBlock, KeyValue, Mono, Page, Skeleton, Split } from "@/lib/ui";
import { PanelEmpty, RoleBadge, useMetaTrail, useParam } from "./shared";

export default function MemberPage() {
  const id = useParam("id");
  const confirmRemoval = useParam("confirm_removal") === "yes";
  const membership = useRead<Row>("get_membership", { id });
  const m = membership.data;
  const access = useRead<Row>(m ? "entity_effective_access" : null, {
    object_type: m?.team_id ? "team" : "organization",
    id: m?.team_id || m?.organization_id,
  });
  useMetaTrail(m?.principal_taap_id, "Member Settings");

  if (membership.isError) {
    return (
      <Page eyebrow="Organization" title="Member">
        <ErrorBlock error={membership.error} retry={() => void membership.refetch()} what="the member" />
      </Page>
    );
  }
  if (!m) {
    return (
      <Page eyebrow="Organization" title="Member">
        <Skeleton lines={5} />
      </Page>
    );
  }
  const entries = access.data ? ((access.data.items ?? []) as Row[]).filter((entry) => entry.principal_taap_id === m.principal_taap_id) : undefined;
  return (
    <Page
      eyebrow="Organization"
      title="Member"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <Mono short={48}>{m.principal_taap_id}</Mono> <RoleBadge role={m.role} />
        </span>
      }
    >
      <Split
        side={
          <>
            <Card label="Membership">
              <KeyValue
                rows={[
                  ["Principal", <code>{m.principal_taap_id}</code>],
                  ["Role", <RoleBadge role={m.role} />],
                  ["Scope", m.team_id ? <Link to={`/team?id=${encodeURIComponent(m.team_id)}`}>Team</Link> : m.organization_id ? <Link to={`/organization?id=${encodeURIComponent(m.organization_id)}`}>Organization</Link> : null],
                  ["Attestation evidence", <code>{m.attestation_digest}</code>],
                ]}
              />
            </Card>
            <Card label="Change role">
              <ChangeRole id={m.id} role={m.role} />
            </Card>
            <Card label="Remove" tone="exception">
              <RemoveMember id={m.id} confirming={confirmRemoval} />
            </Card>
          </>
        }
      >
        <Card label="Effective access" title={entries ? `${entries.length} entries at this scope.` : undefined} padded={false}>
          {access.isError ? (
            <div className="p-4">
              <ErrorBlock error={access.error} retry={() => void access.refetch()} what="effective access" />
            </div>
          ) : !entries ? (
            <div className="p-4">
              <Skeleton lines={2} />
            </div>
          ) : (
            <DataTable
              rows={entries}
              rowKey={(e, i) => `${e.role}:${i}`}
              empty={
                <div className="p-4">
                  <PanelEmpty>No access entries for this member at this scope.</PanelEmpty>
                </div>
              }
              columns={[
                { key: "role", label: "Role", render: (e) => <RoleBadge role={e.role} /> },
                {
                  key: "actions",
                  label: "Actions",
                  render: (e) => (
                    <span className="flex flex-wrap gap-1">
                      {((e.actions ?? []) as string[]).map((a) => (
                        <Chip key={a}>{a}</Chip>
                      ))}
                    </span>
                  ),
                },
                { key: "source", label: "Source", render: (e) => <span className="text-[12.5px] text-ink-2">{e.inherited ? "inherited" : "direct"}</span> },
              ]}
            />
          )}
        </Card>
      </Split>
    </Page>
  );
}

function ChangeRole({ id, role }: { id: string; role: string }) {
  const toast = useToast();
  // The current role is selected (the server page always showed "member" first).
  const [value, setValue] = useState(role);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    setValue(role);
  }, [role]);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("set_member_role", { id, role: value });
      toast("Role changed");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <form
      className="flex flex-wrap items-end gap-2"
      onSubmit={(e) => {
        e.preventDefault();
        void submit();
      }}
    >
      <label className="field m-0 min-w-0 flex-1">
        <span className="sr-only">Role</span>
        <select name="role" value={value} onChange={(e) => setValue(e.target.value)}>
          <option value="member">member</option>
          <option value="admin">admin</option>
          <option value="owner">owner</option>
        </select>
      </label>
      <Button type="submit" loading={busy} disabled={value === role}>
        Change role
      </Button>
      {error && (
        <p className="field-error m-0 w-full" role="alert">
          {error}
        </p>
      )}
    </form>
  );
}

function RemoveMember({ id, confirming }: { id: string; confirming: boolean }) {
  const navigate = useNavigate();
  const { refresh } = useSession();
  const [step, setStep] = useState(confirming);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    setStep(confirming);
  }, [confirming]);
  const remove = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("remove_member", { id });
      refresh();
      navigate("/organizations");
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <>
      <p className="mt-0 text-[13px] text-ink-2">Revokes the membership and the grants derived from it.</p>
      <Button variant="danger" onClick={() => setStep(true)}>
        <UserMinus size={14} aria-hidden /> Remove member
      </Button>
      {step && (
        <Modal
          title="Remove the member?"
          onClose={() => setStep(false)}
          width={460}
          footer={
            <>
              <Button onClick={() => setStep(false)}>Cancel</Button>
              <Button variant="danger" loading={busy} onClick={() => void remove()}>
                Confirm removal
              </Button>
            </>
          }
        >
          <Banner tone="warn">Removing this member revokes their membership and derived grants immediately.</Banner>
          {error && (
            <p className="error" role="alert">
              {error}
            </p>
          )}
        </Modal>
      )}
    </>
  );
}
