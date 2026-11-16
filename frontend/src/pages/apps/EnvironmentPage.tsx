/** One environment (web_ui.cpp web_ui_environment; serve_environment_page and the four
 * serve_platform_form routes), as a Split: environment variables, domains and deployment history
 * on the left; the current deployment and its logs on the right. The forms call their
 * capabilities and stay on the page. */
import { type FormEvent, useState } from "react";
import { Navigate, useSearchParams } from "react-router-dom";
import { Plus, ScrollText } from "lucide-react";
import { act, useRead, type Row } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { Button, Card, DataTable, ErrorBlock, KeyValue, Mono, Page, Skeleton, Split, Status } from "@/lib/ui";
import { formValues } from "./AppsPage";
import { usePlatformRepository, When } from "./EnvironmentsPage";
import { PageError, RepositoryTabs, shortImage, StaleBanner, text, UrlText, useRunner } from "./shared";

export default function EnvironmentPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const name = params.get("name") ?? "";
  const repository = usePlatformRepository(repositoryId, name);
  const environment = useRead<Row>(repositoryId && name ? "get_environment" : null, { repository_id: repositoryId, environment: name }, { workspace: null });
  const deployments = useRead<{ items?: Row[] }>(repositoryId && name ? "list_deployments" : null, { repository_id: repositoryId, environment: name, limit: 100 }, { workspace: null });
  const runner = useRunner();
  // The TXT record the last create_domain answered (lake#949): the host reaches the operator only
  // once verify_domain finds it.
  const [challenge, setChallenge] = useState<Row | null>(null);
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
  const remove = (key: string, capability: string, input: Row, done: string) => runner.run(key, () => act(capability, { ...scope, ...input }), done);
  const claimDomain = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const element = event.currentTarget;
    const form = formValues(event);
    void runner.run("domain", () => act("create_domain", { ...scope, host: form.host, tls: form.tls }), `Domain ${form.host} claimed`).then((value) => {
      if (value === undefined) return;
      element.reset();
      setChallenge(value as Row);
    });
  };
  const verifyDomain = (host: string) =>
    void runner.run(`verify-${host}`, () => act("verify_domain", { ...scope, host }), `${host} verified and requested`).then((value) => {
      if (value !== undefined) setChallenge(null);
    });
  const record = (challenge?.verification as Row | undefined)?.record as Row | undefined;

  const loading = repository.isLoading || environment.isLoading;
  const failed = repository.isError ? repository : environment.isError ? environment : null;
  const data = environment.data ?? {};
  const deployment = (data.deployment && typeof data.deployment === "object" ? data.deployment : {}) as Row;
  const env = Array.isArray(data.env) ? (data.env as Row[]) : [];
  const domains = Array.isArray(data.domains) ? (data.domains as Row[]) : [];
  const logsHref = `/logs?repository_id=${encodeURIComponent(repositoryId)}&environment=${encodeURIComponent(name)}`;

  return (
    <Page
      eyebrow="Data · Environment"
      title={
        <span className="inline-flex flex-wrap items-center gap-2.5">
          {name} {!loading && !failed && <Status value={text(deployment, "phase")} />}
        </span>
      }
      sub={!loading && !failed ? <UrlText url={text(deployment, "url")} empty="URL pending" /> : undefined}
      actions={
        <Button to={logsHref}>
          <ScrollText size={14} aria-hidden /> View logs
        </Button>
      }
    >
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      {loading ? (
        <Skeleton />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="this environment" />
      ) : (
        <>
          <StaleBanner since={text(data, "stale_since")} />
          <PageError error={runner.error} />
          <Split
            side={
              <>
                <Card label="Status" title="Current Deployment">
                  <KeyValue
                    rows={[
                      ["Phase", <Status key="p" value={text(deployment, "phase")} />],
                      ["Change", <code key="c" className="break-all">{text(deployment, "change")}</code>],
                      ["Image", <code key="i" className="break-all">{text(deployment, "image")}</code>],
                      ["Namespace", <code key="n">{text(deployment, "namespace")}</code>],
                      ["URL", <UrlText key="u" url={text(deployment, "url")} empty="Pending" />],
                    ]}
                  />
                </Card>
                <Card label="Logs" title="Container Logs" actions={<Button to={logsHref}>View logs</Button>}>
                  <p className="muted m-0 text-[13px]">Follow this environment's logs live.</p>
                </Card>
              </>
            }
          >
            <Card title="Environment Variables" padded={false}>
              {env.length === 0 ? (
                <p className="muted m-0 px-4 py-3 text-[13px]">No variables set.</p>
              ) : (
                <DataTable
                  rows={env}
                  rowKey={(row) => text(row, "key")}
                  caption="Environment variables"
                  columns={[
                    { key: "key", label: "Key", sort: (row) => text(row, "key"), render: (row) => <code>{text(row, "key")}</code> },
                    { key: "value", label: "Value", render: (row) => <code className="break-all">{text(row, "value")}</code> },
                    { key: "updated", label: "Updated", render: (row) => <When value={text(row, "updated_at")} /> },
                    {
                      key: "unset",
                      label: "",
                      render: (row) => {
                        const key = text(row, "key");
                        return (
                          <Button loading={runner.busy === `unset-${key}`} onClick={() => void remove(`unset-${key}`, "unset_environment_env", { key }, `Unset ${key}`)}>
                            Unset
                          </Button>
                        );
                      },
                    },
                  ]}
                />
              )}
              <form
                className="row-form grid grid-cols-1 items-end gap-2 border-t border-hairline px-4 py-3 sm:grid-cols-[1fr_1fr_auto]"
                onSubmit={submit("set", "set_environment_env", (form) => `Set ${form.key}`, (form) => ({ key: form.key, value: form.value }))}
              >
                <label>
                  Key <input name="key" required />
                </label>
                <label>
                  Value <input name="value" required />
                </label>
                <Button type="submit" loading={runner.busy === "set"}>
                  <Plus size={14} aria-hidden /> Set variable
                </Button>
              </form>
            </Card>

            <Card title="Domains" padded={false}>
              {domains.length === 0 ? (
                <p className="muted m-0 px-4 py-3 text-[13px]">No custom domains.</p>
              ) : (
                <DataTable
                  rows={domains}
                  rowKey={(row) => text(row, "host")}
                  caption="Domains"
                  columns={[
                    {
                      key: "host",
                      label: "Host",
                      sort: (row) => text(row, "host"),
                      render: (row) => (
                        <div className="min-w-0">
                          <div className="font-semibold text-ink">{text(row, "host")}</div>
                          {text(row, "message") && <div className="text-[12px] text-ink-3">{text(row, "message")}</div>}
                        </div>
                      ),
                    },
                    { key: "tls", label: "TLS", render: (row) => <code>{text(row, "tls")}</code> },
                    { key: "status", label: "Status", render: (row) => <Status value={text(row, "status")} /> },
                    {
                      key: "delete",
                      label: "",
                      render: (row) => {
                        const host = text(row, "host");
                        return (
                          <ConfirmButton
                            variant="danger"
                            title="Delete the Domain?"
                            confirmLabel="Delete domain"
                            confirm={
                              <>
                                <code>{host}</code> stops serving this environment.
                              </>
                            }
                            disabled={runner.busy === `delete-${host}`}
                            onConfirm={() => remove(`delete-${host}`, "delete_domain", { host }, `Deletion of ${host} requested`)}
                          >
                            Delete
                          </ConfirmButton>
                        );
                      },
                    },
                  ]}
                />
              )}
              {challenge && record && (
                <div className="border-t border-hairline px-4 py-3 text-[13px]">
                  <p className="m-0">
                    To prove you control <code>{text(challenge, "host")}</code>, publish this record at your DNS provider, then verify it. Nothing is routed until then.
                  </p>
                  <KeyValue
                    rows={[
                      ["Type", <code key="t">{text(record, "type")}</code>],
                      ["Name", <code key="n" className="break-all">{text(record, "name")}</code>],
                      ["Value", <code key="v" className="break-all">{text(record, "value")}</code>],
                    ]}
                  />
                  <Button loading={runner.busy === `verify-${text(challenge, "host")}`} onClick={() => verifyDomain(text(challenge, "host"))}>
                    Verify
                  </Button>
                </div>
              )}
              <form
                className="row-form grid grid-cols-1 items-end gap-2 border-t border-hairline px-4 py-3 sm:grid-cols-[1fr_10rem_auto]"
                onSubmit={claimDomain}
              >
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
                <Button type="submit" loading={runner.busy === "domain"}>
                  <Plus size={14} aria-hidden /> Create domain
                </Button>
              </form>
            </Card>

            <Card title="Deployment History" padded={false}>
              <DataTable
                rows={deployments.data?.items}
                q={deployments}
                what="deployment history"
                caption="Deployment history"
                rowKey={(_row, index) => String(index)}
                empty={<p className="muted m-0 px-4 py-3 text-[13px]">No deployments recorded.</p>}
                columns={[
                  {
                    key: "change",
                    label: "Change",
                    render: (row) => (
                      <div className="min-w-0">
                        <Mono short={16}>{text(row, "change")}</Mono>
                        {text(row, "message") && <div className="mt-0.5 text-[12px] text-ink-3">{text(row, "message")}</div>}
                      </div>
                    ),
                  },
                  { key: "phase", label: "Phase", render: (row) => <Status value={text(row, "phase")} /> },
                  { key: "image", label: "Image", render: (row) => <Mono>{shortImage(text(row, "image"))}</Mono> },
                  { key: "updated", label: "Updated", render: (row) => <When value={text(row, "updated_at")} /> },
                ]}
              />
            </Card>
          </Split>
        </>
      )}
    </Page>
  );
}
