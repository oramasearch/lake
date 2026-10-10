/** Applications (`/applications`): the identity applications of every space the viewer can see,
 * and the form to register one in a workspace (web_ui_applications, serve_applications_page,
 * serve_application_register). */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { act, listOf, read, useList, type Row } from "@/lib/api";
import { Button, ErrorBlock, Field, Form, Page, Skeleton, fieldError } from "@/lib/ui";
import { useMetaTrail } from "@/pages/organizations/shared";

/** "a\r\nb\n\n" → ["a", "b"], as the handler's form_lines split a textarea. */
export function lines(value: string): string[] {
  return value
    .split("\n")
    .map((line) => line.replace(/\r/g, ""))
    .filter((line) => line !== "");
}

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

  const failed = customers.isError ? customers : applications.find((q) => q.isError);
  let table;
  if (failed) table = <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="applications" />;
  else if (customers.isLoading || !listed) table = <Skeleton />;
  else
    table = (
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Pool</th>
              <th>Domain</th>
            </tr>
          </thead>
          <tbody>
            {listed.map((application) => (
              <tr key={application.id}>
                <td>
                  {/* An application whose settings are not in its workspace yet is listed by its id. */}
                  <Link to={`/application?id=${encodeURIComponent(application.id)}`} title={application.settings_unavailable}>
                    {application.name || application.id}
                  </Link>
                </td>
                <td>
                  <code>{application.pool_id}</code>
                </td>
                <td>{application.custom_sign_in_domain || "lake domain"}</td>
              </tr>
            ))}
            {listed.length === 0 && (
              <tr>
                <td colSpan={3} className="empty">
                  No applications.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    );

  // Every live workspace of every listed space, labelled "<space> / <workspace>".
  const choices: { id: string; customerId: string; label: string }[] = [];
  workspaces.forEach((q, index) => {
    const customer = customers.data?.[index];
    for (const workspace of q.data ?? []) {
      if (workspace.desired === "deleted") continue;
      const space = customer?.name ?? "";
      choices.push({ id: workspace.id, customerId: workspace.customer_id ?? customer?.id, label: space ? `${space} / ${workspace.name}` : workspace.name });
    }
  });

  return (
    <Page eyebrow="auth as a service" title="Applications">
      <div className="grid">
        {table}
        <RegisterApplication choices={choices} loading={workspaces.some((q) => q.isLoading)} />
      </div>
    </Page>
  );
}

function RegisterApplication({ choices, loading }: { choices: { id: string; customerId: string; label: string }[]; loading: boolean }) {
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
    <div className="panel">
      <h2>Register Application</h2>
      <Form onSubmit={submit} error={placed ? null : error}>
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
          <textarea name="allowed_origins" value={form.allowed_origins} onChange={set("allowed_origins")} />
        </Field>
        <Field label="Redirect URIs (one per line)" error={errors.redirect_uris}>
          <textarea name="redirect_uris" value={form.redirect_uris} onChange={set("redirect_uris")} />
        </Field>
        <Field label="Custom sign-in domain" error={errors.custom_sign_in_domain}>
          <input name="custom_sign_in_domain" value={form.custom_sign_in_domain} onChange={set("custom_sign_in_domain")} />
        </Field>
        <Field label="Accent color">
          <input name="accent_color" value={form.accent_color} onChange={set("accent_color")} />
        </Field>
        <div>
          <Button variant="primary" type="submit" loading={busy}>
            Register
          </Button>
        </div>
      </Form>
    </div>
  );
}
