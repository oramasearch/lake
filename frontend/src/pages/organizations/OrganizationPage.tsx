// SPDX-License-Identifier: MIT

/** Organization settings (`/organization?id=&tab=`): overview, teams, members, invitations,
 * access, SSO, applications, usage and audit, each a tab in the query (web_ui_organization,
 * serve_organization_page and the team, invitation and SSO form handlers). */
import { useState } from "react";
import { Link, Navigate, useNavigate } from "react-router-dom";
import { HardDrive, Mail, Plus, ScrollText, Timer, Trash2, UserPlus, Users, UsersRound } from "lucide-react";
import { act, ApiError, useList, useRead, type Row } from "@/lib/api";
import { bytes, plural } from "@/lib/format";
import { usageDuration } from "@/pages/admin/usageFormat";
import { Drawer, Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { organizationBlockers, organizationLines, organizationTyped, invitationLines, REVOKE_WORD } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import {
  Button,
  Card,
  DataTable,
  ErrorBlock,
  Field,
  FilterChips,
  Form,
  KeyValue,
  Meter,
  Mono,
  Page,
  SearchInput,
  Skeleton,
  Split,
  Stat,
  StatRow,
  Status,
  Toolbar,
  fieldError,
  type Column,
} from "@/lib/ui";
import { HANDLE_PATTERN, NameCell, ObjectChip, PanelEmpty, QueryTabs, RoleBadge, When, countBy, matches, useMetaTrail, useParam } from "./shared";

const TABS: [string, string][] = [
  ["overview", "Overview"],
  ["members", "Members"],
  ["teams", "Teams"],
  ["invitations", "Invitations"],
  ["access", "Access"],
  ["sso", "SSO"],
  ["applications", "Applications"],
  ["metering", "Usage"],
  ["audit", "Audit"],
];

/** An invitation's `expires_at` from the form's day count, as serve_organization_invitation
 * computed it: a whole number of days from 1 to 90, from now, in UTC to the second. */
export function invitationExpiry(days: string, now = Date.now()): string {
  const count = days === "" ? 7 : Number(days);
  if (!Number.isInteger(count) || count < 1 || count > 90) {
    throw new ApiError(400, "invalid_input", "an invitation lasts from 1 to 90 days", "/expires_in_days");
  }
  return new Date(now + count * 86_400_000).toISOString().replace(/\.\d{3}Z$/, "Z");
}

export default function OrganizationPage() {
  const id = useParam("id");
  const tab = useParam("tab") || "overview";
  const { viewer } = useSession();
  const navigate = useNavigate();
  const organization = useRead<Row>(id ? "get_organization" : null, { id });
  const teams = useList<Row>(id ? "list_teams" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  const memberships = useList<Row>(id ? "list_memberships" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  // Admin-only: a plain member sees its 403 in the invitations tab, not instead of the page.
  const invitations = useList<Row>(id && (tab === "overview" || tab === "invitations") ? "list_invitations" : null, { organization_id: id, limit: 500 }, { pick: "items" });
  const [inviting, setInviting] = useState(false);
  useMetaTrail(organization.data?.display_name, "Organization Settings");

  if (!id) return <Navigate to="/organizations" replace />;
  const fatal = [organization, teams, memberships].find((q) => q.isError);
  if (fatal) {
    return (
      <Page eyebrow="Organization" title="Organization Settings">
        <ErrorBlock error={fatal.error} retry={() => void fatal.refetch()} what="the organization" />
      </Page>
    );
  }
  if (!organization.data || !teams.data || !memberships.data) {
    return (
      <Page eyebrow="Organization" title="Organization Settings">
        <Skeleton lines={6} />
      </Page>
    );
  }
  const org = organization.data;
  const base = `/organization?id=${encodeURIComponent(id)}&tab=`;
  const own = memberships.data.find((m) => !m.team_id && viewer.taap_id && m.principal_taap_id === viewer.taap_id);
  const counts: Record<string, number | undefined> = {
    members: memberships.data.length,
    teams: teams.data.length,
    invitations: invitations.data?.length,
  };
  return (
    <Page
      eyebrow="Organization"
      title={org.display_name}
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          {own ? <RoleBadge role={own.role} /> : null}
          <span>{own ? "Your role here." : "Organization administration, identity, access and evidence."}</span>
        </span>
      }
      actions={
        <>
          {own?.role === "owner" || viewer.administrator ? (
            <DeletionButton
              capability="delete_organization"
              input={{ organization_id: id }}
              workspace={null}
              title={`Delete ${org.display_name || "Organization"}`}
              confirmLabel="Delete organization"
              intro="Deleting the organization removes everything below. It cannot be undone."
              typed={organizationTyped}
              sendConfirm
              lines={organizationLines}
              blocked={(plan) => (organizationBlockers(plan).length > 0 ? <WorkspacesInTheWay plan={plan} /> : null)}
              done={`Deleted ${org.display_name || "the organization"}`}
              onDone={() => navigate("/organizations")}
            >
              <Trash2 size={14} aria-hidden /> Delete organization
            </DeletionButton>
          ) : null}
          <Button variant="primary" onClick={() => setInviting(true)}>
            <UserPlus size={14} aria-hidden /> Invite people
          </Button>
        </>
      }
    >
      <QueryTabs label="Organization settings" tabs={TABS.map(([name, label]) => ({ to: base + name, label, active: tab === name, count: counts[name] }))} />
      {tab === "overview" && <Overview org={org} base={base} teams={teams.data} memberships={memberships.data} invitations={invitations} />}
      {tab === "teams" && <TeamsTab organizationId={id} teams={teams.data} memberships={memberships.data} />}
      {tab === "members" && <MembersTab teams={teams.data} memberships={memberships.data} onInvite={() => setInviting(true)} />}
      {tab === "invitations" && <InvitationsTab invitations={invitations} onInvite={() => setInviting(true)} />}
      {tab === "access" && <AccessTab organizationId={id} />}
      {tab === "sso" && <SsoTab organizationId={id} />}
      {tab === "applications" && <ApplicationsTab organizationId={id} />}
      {tab === "metering" && <UsageTab organizationId={id} />}
      {tab === "audit" && <AuditTab organizationId={id} />}
      {inviting && <InviteDialog organizationId={id} onClose={() => setInviting(false)} />}
    </Page>
  );
}

/** The workspaces an organization still owns, each linked to its deletion: the organization is
 * deleted only once there are none (docs/ui/deletion.md). */
function WorkspacesInTheWay({ plan }: { plan: Row }) {
  const workspaces = organizationBlockers(plan);
  if (workspaces.length === 0) return null;
  return (
    <>
      <strong className="block">Delete its {workspaces.length === 1 ? "workspace" : `${workspaces.length} workspaces`} first</strong>
      The organization still owns {workspaces.length === 1 ? "this workspace" : "these workspaces"}; each holds data that is deleted with it.
      <ul className="mt-2 mb-0 space-y-1 pl-4">
        {workspaces.map((workspace) => (
          <li key={workspace.id}>
            <Link to={`/workspaces?delete=${encodeURIComponent(workspace.id)}`}>{workspace.name}</Link>
            {workspace.environment ? <span className="text-ink-2"> ({workspace.environment})</span> : null}
          </li>
        ))}
      </ul>
    </>
  );
}

function Overview({ org, base, teams, memberships, invitations }: { org: Row; base: string; teams: Row[]; memberships: Row[]; invitations: ReturnType<typeof useList<Row>> }) {
  const id = encodeURIComponent(org.id);
  const owners = memberships.filter((m) => !m.team_id && m.role === "owner").length;
  return (
    <>
      <StatRow cols={3}>
        <Stat label="Members" value={memberships.length} icon={Users} to={base + "members"} sub={`${plural(owners, "owner")}`} />
        <Stat label="Teams" value={teams.length} icon={UsersRound} to={base + "teams"} />
        <Stat
          label="Pending invitations"
          value={invitations.isLoading ? "…" : invitations.data ? invitations.data.length : "—"}
          icon={Mail}
          to={base + "invitations"}
          sub={invitations.isError ? "admins only" : undefined}
        />
      </StatRow>
      <Split
        side={
          <Card label="Pages">
            <ul className="m-0 list-none space-y-1.5 p-0 text-[13px]">
              <li>
                <Link to={`/teams?organization_id=${id}`}>Teams</Link>
              </li>
              <li>
                <Link to={`/members?organization_id=${id}`}>Members and their attestations</Link>
              </li>
              <li>
                <Link to={`/invitations?organization_id=${id}`}>Invitations</Link>
              </li>
              <li>
                <Link to={`/organization/access?id=${id}`}>Effective access</Link>
              </li>
              <li>
                <Link to={`/sso?organization_id=${id}`}>Enterprise SSO and provisioning log</Link>
              </li>
            </ul>
          </Card>
        }
      >
        <Card label="Organization" title={org.display_name}>
          <KeyValue
            rows={[
              ["Identity", org.taap_id ? <code>{org.taap_id}</code> : null],
              ["ID", <code>{org.id}</code>],
              ["Teams", teams.length],
              ["Members", memberships.length],
              ["Pending invitations", invitations.isLoading ? "…" : invitations.data ? invitations.data.length : "—"],
              ["Created", <When value={org.created_at} />],
            ]}
          />
        </Card>
      </Split>
    </>
  );
}

function TeamsTab({ organizationId, teams, memberships }: { organizationId: string; teams: Row[]; memberships: Row[] }) {
  const [search, setSearch] = useState("");
  const [creating, setCreating] = useState(false);
  const shown = teams.filter((t) => matches(search, t.name, t.display_name));
  const size = (teamId: string) => memberships.filter((m) => m.team_id === teamId).length;
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search teams…" />
        <div className="ml-auto">
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New team
          </Button>
        </div>
      </Toolbar>
      <DataTable
        rows={shown}
        rowKey={(t) => t.id}
        empty={
          teams.length === 0 ? (
            <PanelEmpty
              title="No teams yet"
              action={
                <Button variant="primary" onClick={() => setCreating(true)}>
                  New team
                </Button>
              }
            >
              No teams in this organization yet. A team groups members so access can be granted to all of them at once.
            </PanelEmpty>
          ) : (
            <PanelEmpty title="No team matches">Clear the search to see every team.</PanelEmpty>
          )
        }
        columns={[
          {
            key: "team",
            label: "Team",
            sort: (t) => t.display_name || t.name,
            render: (t) => <NameCell to={`/team?id=${encodeURIComponent(t.id)}`} name={t.display_name || t.name} sub={<code>{t.name}</code>} />,
          },
          { key: "members", label: "Members", numeric: true, sort: (t) => size(t.id), render: (t) => <span className="font-mono">{size(t.id)}</span> },
          {
            key: "links",
            label: <span className="sr-only">Links</span>,
            render: (t) => (
              <span className="flex flex-wrap gap-3 text-[12.5px]">
                <Link to={`/members?organization_id=${encodeURIComponent(organizationId)}&team_id=${encodeURIComponent(t.id)}`}>members</Link>
                <Link to={`/team/access?id=${encodeURIComponent(t.id)}`}>effective access</Link>
              </span>
            ),
          },
          { key: "created", label: "Created", sort: (t) => t.created_at, render: (t) => <When value={t.created_at} /> },
        ]}
      />
      {creating && <CreateTeam organizationId={organizationId} onClose={() => setCreating(false)} />}
    </>
  );
}

function CreateTeam({ organizationId, onClose }: { organizationId: string; onClose: () => void }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const input: Row = { organization_id: organizationId, name };
      if (displayName) input.display_name = displayName;
      await act("create_team", input);
      toast("Team created");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const nameError = fieldError(error, "name");
  return (
    <Modal
      title="New Team"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="create-team" loading={busy}>
            Create team
          </Button>
        </>
      }
    >
      <Form id="create-team" onSubmit={submit} error={nameError ? null : error}>
        <Field label="Team name" required error={nameError} help="Lowercase letters, numbers and inner hyphens.">
          <input name="name" pattern={HANDLE_PATTERN} placeholder="reviewers" value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
        <Field label="Display name">
          <input name="display_name" value={displayName} onChange={(e) => setDisplayName(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

function MembersTab({ teams, memberships, onInvite }: { teams: Row[]; memberships: Row[]; onInvite: () => void }) {
  const [search, setSearch] = useState("");
  const [role, setRole] = useState<string | null>(null);
  const teamName = (teamId: string) => {
    const t = teams.find((x) => x.id === teamId);
    return t ? t.display_name || t.name : teamId;
  };
  const shown = memberships.filter((m) => matches(search, m.principal_taap_id, m.principal_type) && (!role || m.role === role));
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search members…" />
        <FilterChips options={countBy(memberships, (m) => m.role)} value={role} onChange={setRole} allLabel="Any role" />
      </Toolbar>
      <DataTable
        rows={shown}
        rowKey={(m) => m.id}
        empty={
          memberships.length === 0 ? (
            <PanelEmpty
              title="No members yet"
              action={
                <Button variant="primary" onClick={onInvite}>
                  Invite people
                </Button>
              }
            >
              Invite people by email address or handle.
            </PanelEmpty>
          ) : (
            <PanelEmpty title="No member matches">Clear the search or the role filter to see more.</PanelEmpty>
          )
        }
        columns={[
          {
            key: "principal",
            label: "Member",
            sort: (m) => m.principal_taap_id,
            render: (m) => <NameCell to={`/member?id=${encodeURIComponent(m.id)}`} name={<Mono short={34}>{m.principal_taap_id}</Mono>} sub={m.principal_type} />,
          },
          { key: "role", label: "Role", sort: (m) => m.role, render: (m) => <RoleBadge role={m.role} /> },
          { key: "scope", label: "Scope", render: (m) => (m.team_id ? <Link to={`/team?id=${encodeURIComponent(m.team_id)}`}>{teamName(m.team_id)}</Link> : <span className="text-ink-3">Organization</span>) },
          { key: "attestation", label: "Attestation", render: (m) => <Mono short={12}>{m.attestation_digest}</Mono> },
        ]}
      />
    </>
  );
}

function InvitationsTab({ invitations, onInvite }: { invitations: ReturnType<typeof useList<Row>>; onInvite: () => void }) {
  const [search, setSearch] = useState("");
  const [status, setStatus] = useState<string | null>(null);
  const rows = invitations.data;
  const shown = rows?.filter((i) => matches(search, i.invitee, i.invitee_type) && (!status || i.status === status));
  const columns: Column<Row>[] = [
    { key: "invitee", label: "Invitee", sort: (i) => i.invitee, render: (i) => <NameCell name={i.invitee} sub={i.invitee_type} /> },
    { key: "role", label: "Role", render: (i) => <RoleBadge role={i.role} /> },
    { key: "status", label: "Status", render: (i) => <Status value={i.status} /> },
    { key: "expires", label: "Expires", render: (i) => <When value={i.expires_at} />, sort: (i) => i.expires_at },
    { key: "revoke", label: "", render: (i) => <RevokeInvitation invitation={i} /> },
  ];
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search invitations…" />
        <FilterChips options={countBy(rows, (i) => i.status)} value={status} onChange={setStatus} allLabel="Any status" />
      </Toolbar>
      <DataTable
        q={invitations}
        what="invitations"
        rows={shown}
        columns={columns}
        rowKey={(i) => i.id}
        empty={
          (rows?.length ?? 0) === 0 ? (
            <PanelEmpty
              title="No invitations yet"
              action={
                <Button variant="primary" onClick={onInvite}>
                  Invite people
                </Button>
              }
            >
              Invitations you send appear here until they are accepted or expire.
            </PanelEmpty>
          ) : (
            <PanelEmpty title="No invitation matches">Clear the search or the status filter to see more.</PanelEmpty>
          )
        }
      />
    </>
  );
}

/** Revoke, on a pending invitation that has not expired: the one an invitee could still accept. */
export function RevokeInvitation({ invitation }: { invitation: Row }) {
  if (invitation.status !== "pending" || (invitation.expires_at && Date.parse(invitation.expires_at) <= Date.now())) return null;
  return (
    <DeletionButton
      variant="ghost"
      capability="revoke_invitation"
      input={{ invitation_id: invitation.id }}
      workspace={null}
      title={`Revoke the Invitation to ${invitation.invitee}`}
      confirmLabel="Revoke invitation"
      typed={() => REVOKE_WORD}
      lines={invitationLines}
      done="Invitation revoked"
    >
      Revoke
    </DeletionButton>
  );
}

function InviteDialog({ organizationId, onClose }: { organizationId: string; onClose: () => void }) {
  const toast = useToast();
  const [invitee, setInvitee] = useState("");
  const [inviteeType, setInviteeType] = useState("email");
  const [role, setRole] = useState("member");
  const [days, setDays] = useState("7");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const expiresAt = invitationExpiry(days);
      await act("create_invitation", { organization_id: organizationId, invitee_type: inviteeType, invitee, role, expires_at: expiresAt });
      setInvitee("");
      toast("Invitation sent");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const inviteeError = fieldError(error, "invitee");
  const daysError = fieldError(error, "expires_in_days") ?? fieldError(error, "expires_at");
  return (
    <Modal
      title="Invite a Member"
      subtitle="They join from their Organizations page once they sign in with that email address, or with the workspace that has that handle."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="invite-member" loading={busy}>
            Send invitation
          </Button>
        </>
      }
    >
      <Form id="invite-member" onSubmit={submit} error={inviteeError || daysError ? null : error}>
        <Field label="Email or handle" required error={inviteeError}>
          <input name="invitee" value={invitee} onChange={(e) => setInvitee(e.target.value)} autoFocus />
        </Field>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <Field label="Type">
            <select name="invitee_type" value={inviteeType} onChange={(e) => setInviteeType(e.target.value)}>
              <option value="email">Email</option>
              <option value="handle">Handle</option>
            </select>
          </Field>
          <Field label="Expires in" error={daysError}>
            <select name="expires_in_days" value={days} onChange={(e) => setDays(e.target.value)}>
              <option value="1">1 day</option>
              <option value="7">7 days</option>
              <option value="30">30 days</option>
            </select>
          </Field>
        </div>
        <Field label="Role">
          <select name="role" value={role} onChange={(e) => setRole(e.target.value)}>
            <option value="member">Member: can read the organization</option>
            <option value="admin">Admin: can manage people, teams and access</option>
            <option value="owner">Owner: can manage; an organization always keeps at least one</option>
          </select>
        </Field>
      </Form>
    </Modal>
  );
}

function AccessTab({ organizationId }: { organizationId: string }) {
  const access = useRead<Row>("entity_effective_access", { object_type: "organization", id: organizationId });
  const [search, setSearch] = useState("");
  const items: Row[] | undefined = access.data ? ((access.data.items ?? []) as Row[]).filter((e) => matches(search, e.principal_taap_id, e.role)) : undefined;
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search principals…" />
        <div className="ml-auto">
          <Button to={`/organization/access?id=${encodeURIComponent(organizationId)}`}>Open effective access</Button>
        </div>
      </Toolbar>
      <DataTable
        q={access}
        what="effective access"
        rows={items}
        rowKey={(e, i) => `${e.principal_taap_id}:${e.role}:${i}`}
        empty={<PanelEmpty title="No effective access entries">Nobody reaches this organization yet.</PanelEmpty>}
        columns={[
          { key: "principal", label: "Principal", sort: (e) => e.principal_taap_id, render: (e) => <Mono short={40}>{e.principal_taap_id}</Mono> },
          { key: "role", label: "Role", sort: (e) => e.role, render: (e) => <RoleBadge role={e.role} /> },
          { key: "source", label: "Source", render: (e) => (e.source_object_id ? <span className="text-ink-2">{e.inherited ? "inherited" : "direct"}</span> : <span className="muted">—</span>) },
        ]}
      />
    </>
  );
}

function SsoTab({ organizationId }: { organizationId: string }) {
  const providers = useList<Row>("list_sso_providers", { organization_id: organizationId, limit: 100 }, { pick: "items" });
  const [adding, setAdding] = useState(false);
  return (
    <>
      <Toolbar>
        <span className="text-[13px] text-ink-2">Provider secrets and SCIM tokens are never shown.</span>
        <div className="ml-auto flex flex-wrap gap-2">
          <Button to={`/sso?organization_id=${encodeURIComponent(organizationId)}`}>
            <ScrollText size={14} aria-hidden /> Provisioning log
          </Button>
          <Button variant="primary" onClick={() => setAdding(true)}>
            <Plus size={14} aria-hidden /> Add provider
          </Button>
        </div>
      </Toolbar>
      <DataTable
        q={providers}
        what="SSO providers"
        rows={providers.data}
        rowKey={(p) => p.id}
        empty={
          <PanelEmpty
            title="No SSO providers"
            action={
              <Button variant="primary" onClick={() => setAdding(true)}>
                Add provider
              </Button>
            }
          >
            Connect an OpenID Connect or SAML identity provider so members sign in with it.
          </PanelEmpty>
        }
        columns={[
          {
            key: "name",
            label: "Provider",
            render: (p) => <NameCell to={`/sso?organization_id=${encodeURIComponent(organizationId)}&provider_id=${encodeURIComponent(p.id)}`} name={p.name} sub={p.issuer} />,
          },
          { key: "protocol", label: "Protocol", render: (p) => <span className="font-mono text-[12px] uppercase">{p.protocol}</span> },
          { key: "tested", label: "Tested", render: (p) => (p.tested_at ? <When value={p.tested_at} /> : <Status value="not tested" tone="warning" />) },
        ]}
      />
      {adding && <SsoProviderDrawer organizationId={organizationId} onClose={() => setAdding(false)} />}
    </>
  );
}

function SsoProviderDrawer({ organizationId, onClose }: { organizationId: string; onClose: () => void }) {
  const toast = useToast();
  const [form, setForm] = useState({ name: "", protocol: "oidc", issuer: "", scim_token: "" });
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const set = (key: keyof typeof form) => (e: { target: { value: string } }) => setForm((f) => ({ ...f, [key]: e.target.value }));
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("register_sso_provider", { organization_id: organizationId, ...form });
      toast("Provider saved");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const errors = { name: fieldError(error, "name"), issuer: fieldError(error, "issuer"), scim_token: fieldError(error, "scim_token") };
  return (
    <Drawer
      title="Configure SSO Provider"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="sso-provider" loading={busy}>
            Save provider
          </Button>
        </>
      }
    >
      <Form id="sso-provider" onSubmit={submit} error={errors.name || errors.issuer || errors.scim_token ? null : error}>
        <Field label="Name" required error={errors.name}>
          <input name="name" value={form.name} onChange={set("name")} autoFocus />
        </Field>
        <Field label="Protocol">
          <select name="protocol" value={form.protocol} onChange={set("protocol")}>
            <option value="oidc">OpenID Connect</option>
            <option value="saml">SAML 2.0</option>
          </select>
        </Field>
        <Field label="Issuer" required error={errors.issuer}>
          <input name="issuer" value={form.issuer} onChange={set("issuer")} />
        </Field>
        <Field label="SCIM token" required error={errors.scim_token}>
          <input name="scim_token" autoComplete="off" value={form.scim_token} onChange={set("scim_token")} />
        </Field>
      </Form>
    </Drawer>
  );
}

/** The organization's own workspace: no capability maps an organization to it, so find the
 * customer whose `organization_id` is this one, as the server page did. */
function useOrganizationSpace(organizationId: string) {
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  const space = customers.data?.find((customer) => customer.organization_id === organizationId);
  return { customers, space };
}

function ApplicationsTab({ organizationId }: { organizationId: string }) {
  const { customers, space } = useOrganizationSpace(organizationId);
  const applications = useList<Row>(space ? "list_applications" : null, { customer_id: space?.id, limit: 100 }, { pick: "items" });
  const failed = customers.isError ? customers : applications.isError ? applications : null;
  if (customers.isLoading || applications.isLoading) return <Skeleton />;
  if (failed) return <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="applications" />;
  const rows = applications.data ?? [];
  if (rows.length === 0) {
    return (
      <PanelEmpty title="No applications" action={<Button to="/applications">Applications</Button>}>
        No applications belong to this organization customer.
      </PanelEmpty>
    );
  }
  return (
    <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
      {rows.map((application) => (
        <Card key={application.id} label="Application" title={<Link to={`/application?id=${encodeURIComponent(application.id)}`}>{application.name || application.id}</Link>}>
          <KeyValue
            rows={[
              ["Pool", application.pool_id ? <Mono short={20}>{application.pool_id}</Mono> : null],
              ["Domain", application.custom_sign_in_domain || "Lake domain"],
            ]}
          />
        </Card>
      ))}
    </div>
  );
}

function UsageTab({ organizationId }: { organizationId: string }) {
  const { customers, space } = useOrganizationSpace(organizationId);
  const usage = useRead<Row>(space ? "metering_usage" : null, { customer_id: space?.id });
  const failed = customers.isError ? customers : usage.isError ? usage : null;
  if (customers.isLoading || usage.isLoading) return <Skeleton />;
  if (failed) return <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="usage" />;
  if (!space) return <PanelEmpty title="No workspace">No workspace is attached to this organization.</PanelEmpty>;
  const quota = Number(usage.data?.quota_bytes ?? 0);
  const storage: Row = usage.data?.summary?.storage ?? {};
  const total = storage.total_bytes ?? 0;
  const repositories = Number(storage.repositories_bytes ?? 0);
  return (
    <>
      <StatRow cols={3}>
        <Stat label="Stored" value={bytes(total)} icon={HardDrive} sub="repositories, images and workspace data" />
        <Stat label="Repository storage limit" value={quota === 0 ? "No limit" : bytes(quota)} sub={quota === 0 ? "writes are never refused for size" : <Meter value={repositories} max={quota} label={`${Math.round((repositories / quota) * 100)}%`} tone={repositories > quota ? "bad" : undefined} />} />
        <Stat label="Compute" value={usageDuration(usage.data?.summary?.compute?.total_seconds)} icon={Timer} sub="builds and workspace runs" />
      </StatRow>
      <Button variant="primary" to={`/metering?customer_id=${encodeURIComponent(space.id)}`}>
        Open usage
      </Button>
    </>
  );
}

function AuditTab({ organizationId }: { organizationId: string }) {
  // The organization's account audit (lake#18): the central events attributed to its account and
  // its workspaces' audits, which the organization's administrators may read. Anyone else is shown
  // the refusal in the table.
  const { customers, space } = useOrganizationSpace(organizationId);
  const audit = useList<Row>(space ? "audit" : null, { customer_id: space?.id, limit: 200 }, { pick: "items" });
  const [outcome, setOutcome] = useState<string | null>(null);
  const [search, setSearch] = useState("");
  if (customers.isLoading) return <Skeleton />;
  if (customers.isError) return <ErrorBlock error={customers.error} retry={() => void customers.refetch()} what="the organization's account" />;
  if (!space) return <PanelEmpty title="No account">This organization has no account, so it has no audit of its own.</PanelEmpty>;
  const shown = audit.data?.filter((e) => (!outcome || e.outcome === outcome) && matches(search, e.action, e.capability, e.principal, e.principal_name));
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search actions…" />
        <FilterChips options={countBy(audit.data, (e) => e.outcome)} value={outcome} onChange={setOutcome} allLabel="Any outcome" />
        <div className="ml-auto flex flex-wrap items-center gap-2">
          <Button href={`/v1/export_audit?limit=1000&customer_id=${encodeURIComponent(space.id)}`} title="The account's events as JSON lines (export_audit)">
            Export
          </Button>
        </div>
      </Toolbar>
      <DataTable
        q={audit}
        what="audit events"
        rows={shown}
        rowKey={(e) => e.id}
        empty={<PanelEmpty title="No matching audit events">Changes to this organization and its workspaces are recorded here.</PanelEmpty>}
        columns={[
          { key: "when", label: "When", sort: (e) => e.occurred_at, render: (e) => <When value={e.occurred_at} /> },
          { key: "action", label: "Action", render: (e) => <NameCell name={<code>{e.action}</code>} sub={e.capability} /> },
          { key: "principal", label: "By", render: (e) => (e.principal_name && e.principal_name !== e.principal ? <span title={e.principal}>{e.principal_name}</span> : <Mono short={24}>{e.principal}</Mono>) },
          { key: "object", label: "Object", render: (e) => <ObjectChip value={e.object} /> },
          { key: "outcome", label: "Outcome", render: (e) => <Status value={e.outcome} /> },
          { key: "log", label: "Log", render: (e) => (e.log === "workspace" ? <Mono short={12}>{e.workspace_id}</Mono> : "Central") },
        ]}
      />
    </>
  );
}
