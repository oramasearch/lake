/** Member settings (`/member?id=&confirm_removal=yes`): one membership, its role and its removal
 * in two steps (web_ui_member, serve_member_page, serve_member_role, serve_member_remove). */
import { useEffect, useState } from "react";
import { useNavigate } from "react-router-dom";
import { act, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { Panel, useMetaTrail, useParam } from "./shared";

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
      <Page eyebrow="organization settings" title="Member">
        <ErrorBlock error={membership.error} retry={() => void membership.refetch()} what="the member" />
      </Page>
    );
  }
  if (!m) {
    return (
      <Page eyebrow="organization settings" title="Member">
        <Skeleton />
      </Page>
    );
  }
  const entries = ((access.data?.items ?? []) as Row[]).filter((entry) => entry.principal_taap_id === m.principal_taap_id);
  return (
    <Page eyebrow="organization settings" title="Member">
      <Panel>
        <dl>
          <dt>Principal</dt>
          <dd>
            <code>{m.principal_taap_id}</code>
          </dd>
          <dt>Role</dt>
          <dd>{m.role}</dd>
          <dt>Attestation evidence</dt>
          <dd>
            <code>{m.attestation_digest}</code>
          </dd>
        </dl>
        <ChangeRole id={m.id} role={m.role} />
        <RemoveMember id={m.id} confirming={confirmRemoval} />
      </Panel>
      <Panel title="Effective Access">
        {access.isError ? (
          <ErrorBlock error={access.error} retry={() => void access.refetch()} what="effective access" />
        ) : (
          <p>{access.data ? `${entries.length} entries at this scope.` : "…"}</p>
        )}
      </Panel>
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
      className="inline"
      onSubmit={(e) => {
        e.preventDefault();
        void submit();
      }}
    >
      <label>
        Role{" "}
        <select name="role" value={value} onChange={(e) => setValue(e.target.value)}>
          <option value="member">member</option>
          <option value="admin">admin</option>
          <option value="owner">owner</option>
        </select>
      </label>{" "}
      <Button type="submit" loading={busy}>
        Change role
      </Button>
      {error && (
        <span className="field-error" role="alert">
          {" "}
          {error}
        </span>
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
  if (!step) {
    return (
      <p>
        <Button variant="danger" onClick={() => setStep(true)}>
          Remove member
        </Button>
      </p>
    );
  }
  return (
    <>
      <p className="notice">Removing this member revokes their membership and derived grants immediately.</p>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      <p className="flex gap-2">
        <Button variant="danger" loading={busy} onClick={() => void remove()}>
          Confirm removal
        </Button>
        <Button onClick={() => setStep(false)}>Cancel</Button>
      </p>
    </>
  );
}
