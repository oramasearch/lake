/** Applications (`/applications`): the identity applications of every customer the viewer can
 * see, as cards, and the drawer to register one in a workspace (web_ui_applications,
 * serve_applications_page, serve_application_register). */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { AppWindow, Globe, Plus, Users } from "lucide-react";
import { act, listOf, read, useList, type Row } from "@/lib/api";
import { plural } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { Button, Card, Empty, ErrorBlock, Field, FilterChips, Form, KeyValue, Mono, Page, SearchInput, Skeleton, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { PanelEmpty, matches, useMetaTrail } from "@/pages/organizations/shared";

/** "a\r\nb\n\n" → ["a", "b"], as the handler's form_lines split a textarea. */
export function lines(value: string): string[] {
  return value
    .split("\n")
    .map((line) => line.replace(/\r/g, ""))
    .filter((line) => line !== "");
}

type Choice = { id: string; customerId: string; label: string };

/** Users are counted per card only while the list is short: one read per application. */
const COUNT_USERS_UP_TO = 24;

export default function ApplicationsPage() {
  const customers = useList<Row>("list_customers", { limit: 500 }, { pick: "items" });
  const ids = (customers.data ?? []).map((customer) => customer.id as string);
  const applications = useQueries({
    queries: ids.map((id) => ({
      // The key useList gives the same read, so the two share one cache entry.
      queryKey: ["list_applications", "", { customer_id: id, limit: 500 }, "items"],
      queryFn: async () => listOf<Row>(await read("list_applications", { customer_id: id, limit: 500 }, { workspace: null }), "items"),
    })),
  });
  const workspaces = useQueries({
    queries: ids.map((id) => ({
      queryKey: ["list_workspaces", "", { customer_id: id }, "workspaces"],
      queryFn: async () => listOf<Row>(await read("list_workspaces", { customer_id: id }, { workspace: null }), "workspaces"),
    })),
  });
  const listed = applications.every((q) => q.data) ? applications.flatMap((q) => q.data ?? []) : null;
  useMetaTrail(customers.data && listed ? `${listed.length} applications` : null, "Applications");
  const [registering, setRegistering] = useState(false);
  const [search, setSearch] = useState("");
  const [domain, setDomain] = useState<"custom" | "lake" | null>(null);

  // Every live workspace of every listed customer, labelled "<customer> / <workspace>".
  const choices: Choice[] = [];
  const workspaceLabels = new Map<string, string>();
  workspaces.forEach((q, index) => {
    const customer = customers.data?.[index];
    for (const workspace of q.data ?? []) {
      const owner = customer?.name ?? "";
      const label = owner ? `${owner} / ${workspace.name}` : workspace.name;
      workspaceLabels.set(workspace.id, label);
      if (workspace.desired === "deleted") continue;
      choices.push({ id: workspace.id, customerId: workspace.customer_id ?? customer?.id, label });
    }
  });

  const failed = customers.isError ? customers : applications.find((q) => q.isError);
  let body;
  if (failed) body = <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="applications" />;
  else if (customers.isLoading || !listed) body = <Skeleton lines={5} />;
  else if (listed.length === 0)
    body = (
      <Empty
        icon={AppWindow}
        title="No applications"
        action={
          <Button variant="primary" onClick={() => setRegistering(true)}>
            <Plus size={14} aria-hidden /> Register application
          </Button>
        }
      >
        An application gives your product hosted sign-in, its own user pool and signed lifecycle webhooks.
      </Empty>
    );
  else {
    const custom = listed.filter((a) => a.custom_sign_in_domain).length;
    const shown = listed.filter(
      (a) => matches(search, a.name, a.id, a.pool_id, a.custom_sign_in_domain, workspaceLabels.get(a.workspace_id)) && (!domain || (domain === "custom") === !!a.custom_sign_in_domain),
    );
    body = (
      <>
        <StatRow cols={3}>
          <Stat label="Applications" value={listed.length} icon={AppWindow} />
          <Stat label="Custom domains" value={custom} icon={Globe} sub="sign in on their own host" />
          <Stat label="Workspaces" value={new Set(listed.map((a) => a.workspace_id).filter(Boolean)).size} sub="hold their events and users" />
        </StatRow>
        <Toolbar>
          <SearchInput value={search} onChange={setSearch} placeholder="Search applications…" />
          <FilterChips
            options={[
              { id: "custom" as const, label: "Custom domain", count: custom },
              { id: "lake" as const, label: "Lake domain", count: listed.length - custom },
            ]}
            value={domain}
            onChange={setDomain}
          />
        </Toolbar>
        {shown.length === 0 ? (
          <PanelEmpty title="No application matches">Clear the search or the filter to see more.</PanelEmpty>
        ) : (
          <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
            {shown.map((application) => (
              <ApplicationCard key={application.id} application={application} workspaceLabel={workspaceLabels.get(application.workspace_id)} countUsers={listed.length <= COUNT_USERS_UP_TO} />
            ))}
          </div>
        )}
      </>
    );
  }

  return (
    <Page
      eyebrow="Administration"
      title="Applications"
      sub="Hosted sign-in and user pools for the products you build on Lake."
      actions={
        <Button variant="primary" onClick={() => setRegistering(true)}>
          <Plus size={14} aria-hidden /> Register application
        </Button>
      }
    >
      {body}
      {registering && <RegisterApplication choices={choices} loading={workspaces.some((q) => q.isLoading)} onClose={() => setRegistering(false)} />}
    </Page>
  );
}

function ApplicationCard({ application, workspaceLabel, countUsers }: { application: Row; workspaceLabel?: string; countUsers: boolean }) {
  const workspaceId: string | undefined = application.workspace_id ?? undefined;
  const users = useList<Row>(countUsers && workspaceId ? "list_application_users" : null, { application_id: application.id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  const app = encodeURIComponent(application.id);
  return (
    <Card
      label="Application"
      // An application whose settings are not in its workspace yet is listed by its id.
      title={
        <Link to={`/application?id=${app}`} title={application.settings_unavailable}>
          {application.name || application.id}
        </Link>
      }
      footer={
        <span className="flex flex-wrap gap-3">
          <Link to={`/application/users?id=${app}`}>users</Link>
          <Link to={`/application/organizations?id=${app}`}>organizations</Link>
          <Link to={`/application/events?id=${app}`}>webhook events</Link>
        </span>
      }
    >
      <KeyValue
        rows={[
          ["Workspace", workspaceId ? (workspaceLabel ?? <Mono short={12}>{workspaceId}</Mono>) : "not assigned"],
          [
            "Users",
            countUsers && workspaceId ? (
              <span className="inline-flex items-center gap-1">
                <Users size={13} className="text-ink-3" aria-hidden />
                {users.isLoading ? "…" : users.isError ? "—" : (users.data?.length ?? 0) === 500 ? "500+" : plural(users.data?.length ?? 0, "user")}
              </span>
            ) : null,
          ],
          ["Pool", <Mono short={18}>{application.pool_id}</Mono>],
          ["Domain", application.custom_sign_in_domain || "Lake domain"],
        ]}
      />
    </Card>
  );
}

function RegisterApplication({ choices, loading, onClose }: { choices: Choice[]; loading: boolean; onClose: () => void }) {
  const navigate = useNavigate();
  const [form, setForm] = useState({ workspace_id: "", name: "", allowed_origins: "", redirect_uris: "", custom_sign_in_domain: "", accent_color: "#0b6e99" });
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const set = (key: keyof typeof form) => (e: { target: { value: string } }) => setForm((f) => ({ ...f, [key]: e.target.value }));
  const workspaceId = form.workspace_id || choices[0]?.id || "";
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const choice = choices.find((c) => c.id === workspaceId);
      const application = await act<Row>("register_application", {
        customer_id: choice?.customerId ?? "",
        workspace_id: workspaceId,
        name: form.name,
        allowed_origins: lines(form.allowed_origins),
        redirect_uris: lines(form.redirect_uris),
        custom_sign_in_domain: form.custom_sign_in_domain,
        theme: { accent_color: form.accent_color },
      });
      onClose();
      navigate(`/application?id=${encodeURIComponent(application.id)}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const errors = {
    workspace_id: fieldError(error, "workspace_id"),
    name: fieldError(error, "name"),
    allowed_origins: fieldError(error, "allowed_origins"),
    redirect_uris: fieldError(error, "redirect_uris"),
    custom_sign_in_domain: fieldError(error, "custom_sign_in_domain"),
  };
  const placed = Object.values(errors).some(Boolean);
  return (
    <Drawer
      title="Register Application"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="register-application" loading={busy}>
            Register
          </Button>
        </>
      }
    >
      <Form id="register-application" onSubmit={submit} error={placed ? null : error}>
        <Field label="Workspace" required error={errors.workspace_id} help="The workspace holds the application's lifecycle events and webhook deliveries, and never changes.">
          <select name="workspace_id" value={workspaceId} onChange={set("workspace_id")} disabled={loading}>
            {choices.map((choice) => (
              <option key={choice.id} value={choice.id}>
                {choice.label}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Name" required error={errors.name}>
          <input name="name" value={form.name} onChange={set("name")} />
        </Field>
        <Field label="Allowed origins (one per line)" required error={errors.allowed_origins}>
          <textarea name="allowed_origins" rows={3} value={form.allowed_origins} onChange={set("allowed_origins")} placeholder="https://app.example.com" />
        </Field>
        <Field label="Redirect URIs (one per line)" error={errors.redirect_uris}>
          <textarea name="redirect_uris" rows={3} value={form.redirect_uris} onChange={set("redirect_uris")} />
        </Field>
        <Field label="Custom sign-in domain" error={errors.custom_sign_in_domain}>
          <input name="custom_sign_in_domain" value={form.custom_sign_in_domain} onChange={set("custom_sign_in_domain")} />
        </Field>
        <Field label="Accent color">
          <input name="accent_color" value={form.accent_color} onChange={set("accent_color")} />
        </Field>
      </Form>
    </Drawer>
  );
}
