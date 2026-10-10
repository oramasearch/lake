/** One environment (web_ui.cpp web_ui_environment; serve_environment_page and the four
 * serve_platform_form routes): the current deployment, environment variables, domains and
 * deployment history. The forms call their capabilities and stay on the page. */
import { type FormEvent } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { act, useRead, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { formValues } from "./AppsPage";
import { usePlatformRepository } from "./EnvironmentsPage";
import { PageError, RepositoryTabs, shortImage, StaleBanner, text, UrlText, useRunner } from "./shared";

export default function EnvironmentPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const name = params.get("name") ?? "";
  const repository = usePlatformRepository(repositoryId, name);
  const environment = useRead<Row>(repositoryId && name ? "get_environment" : null, { repository_id: repositoryId, environment: name }, { workspace: null });
  const deployments = useRead<{ items?: Row[] }>(repositoryId && name ? "list_deployments" : null, { repository_id: repositoryId, environment: name, limit: 100 }, { workspace: null });
  const runner = useRunner();
  if (!repositoryId || !name) return <Navigate to="/repositories" replace />;

  const scope = { repository_id: repositoryId, environment: name };
  const submit = (key: string, capability: string, done: (form: Record<string, string>) => string, input: (form: Record<string, string>) => Row) => (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const element = event.currentTarget;
    const form = formValues(event);
    void runner.run(key, () => act(capability, { ...scope, ...input(form) }), done(form)).then((value) => {
      if (value !== undefined) element.reset();
    });
  };
  const remove = (key: string, capability: string, input: Row, done: string) => void runner.run(key, () => act(capability, { ...scope, ...input }), done);

  const loading = repository.isLoading || environment.isLoading;
  const failed = repository.isError ? repository : environment.isError ? environment : null;
  const data = environment.data ?? {};
  const deployment = (data.deployment && typeof data.deployment === "object" ? data.deployment : {}) as Row;
  const env = Array.isArray(data.env) ? (data.env as Row[]) : [];
  const domains = Array.isArray(data.domains) ? (data.domains as Row[]) : [];

  return (
    <Page eyebrow="environment" title={name}>
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      {loading ? (
        <Skeleton />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="this environment" />
      ) : (
        <>
          <StaleBanner since={text(data, "stale_since")} />
          <PageError error={runner.error} />
          <section className="panel">
            <h2>Current Deployment</h2>
            <dl>
              <dt>Phase</dt>
              <dd>{text(deployment, "phase")}</dd>
              <dt>Change</dt>
              <dd>
                <code>{text(deployment, "change")}</code>
              </dd>
              <dt>Image</dt>
              <dd>
                <code>{text(deployment, "image")}</code>
              </dd>
              <dt>Namespace</dt>
              <dd>
                <code>{text(deployment, "namespace")}</code>
              </dd>
              <dt>URL</dt>
              <dd>
                <UrlText url={text(deployment, "url")} empty="Pending" />
              </dd>
            </dl>
            <p>
              <Link to={`/logs?repository_id=${encodeURIComponent(repositoryId)}&environment=${encodeURIComponent(name)}`}>View logs</Link>
            </p>
          </section>

          <section className="panel">
            <h2>Environment Variables</h2>
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Key</th>
                    <th>Value</th>
                    <th>Updated</th>
                    <th />
                  </tr>
                </thead>
                <tbody>
                  {env.map((row) => {
                    const key = text(row, "key");
                    return (
                      <tr key={key}>
                        <td>
                          <code>{key}</code>
                        </td>
                        <td>
                          <code>{text(row, "value")}</code>
                        </td>
                        <td>{text(row, "updated_at")}</td>
                        <td>
                          <button type="button" disabled={runner.busy === `unset-${key}`} onClick={() => remove(`unset-${key}`, "unset_environment_env", { key }, `Unset ${key}`)}>
                            Unset
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
            <form onSubmit={submit("set", "set_environment_env", (form) => `Set ${form.key}`, (form) => ({ key: form.key, value: form.value }))}>
              <label>
                Key <input name="key" required />
              </label>
              <label>
                Value <input name="value" required />
              </label>
              <button type="submit" disabled={runner.busy === "set"}>
                Set variable
              </button>
            </form>
          </section>

          <section className="panel">
            <h2>Domains</h2>
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Host</th>
                    <th>TLS</th>
                    <th>Status</th>
                    <th>Message</th>
                    <th />
                  </tr>
                </thead>
                <tbody>
                  {domains.map((row) => {
                    const host = text(row, "host");
                    return (
                      <tr key={host}>
                        <td>{host}</td>
                        <td>{text(row, "tls")}</td>
                        <td>
                          <span className="badge">{text(row, "status")}</span>
                        </td>
                        <td>{text(row, "message")}</td>
                        <td>
                          <button type="button" disabled={runner.busy === `delete-${host}`} onClick={() => remove(`delete-${host}`, "delete_domain", { host }, `Deletion of ${host} requested`)}>
                            Delete
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
            <form onSubmit={submit("domain", "create_domain", (form) => `Domain ${form.host} requested`, (form) => ({ host: form.host, tls: form.tls }))}>
              <label>
                Host <input name="host" required />
              </label>
              <label>
                TLS{" "}
                <select name="tls" defaultValue="acme">
                  <option value="acme">ACME</option>
                  <option value="provided">Provided</option>
                </select>
              </label>
              <button type="submit" disabled={runner.busy === "domain"}>
                Create domain
              </button>
            </form>
          </section>

          <section className="panel">
            <h2>Deployment History</h2>
            {deployments.isError ? (
              <ErrorBlock error={deployments.error} retry={() => void deployments.refetch()} what="deployment history" />
            ) : deployments.isLoading ? (
              <Skeleton lines={2} />
            ) : (
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>Phase</th>
                      <th>Change</th>
                      <th>Image</th>
                      <th>Updated</th>
                      <th>Message</th>
                    </tr>
                  </thead>
                  <tbody>
                    {(deployments.data?.items ?? []).map((row, index) => (
                      <tr key={index}>
                        <td>{text(row, "phase")}</td>
                        <td>
                          <code>{text(row, "change")}</code>
                        </td>
                        <td>
                          <code>{shortImage(text(row, "image"))}</code>
                        </td>
                        <td>{text(row, "updated_at")}</td>
                        <td>{text(row, "message")}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </section>
        </>
      )}
    </Page>
  );
}
