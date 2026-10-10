/** Usage (`/metering?customer_id=`): what a space stores and the compute it used, its estimated
 * charge and its repository storage limit (web_ui_metering, serve_metering_page,
 * serve_metering_quota). */
import { useEffect, useState } from "react";
import { act, ApiError, useRead, type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, ErrorBlock, Field, Form, Page, Skeleton, fieldError } from "@/lib/ui";
import { Panel, PanelEmpty, useMetaTrail, useParam, useQuerySubmit } from "@/pages/organizations/shared";

/** "0 s", "45 s", "12 min 5 s", "3 h 4 min": compute time in plain units (usage_duration). */
export function usageDuration(value: unknown): string {
  const seconds = Math.max(0, Math.floor(Number(value) || 0));
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return seconds % 60 === 0 ? `${seconds / 60} min` : `${Math.floor(seconds / 60)} min ${seconds % 60} s`;
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return minutes === 0 ? `${hours} h` : `${hours} h ${minutes} min`;
}

/** Integer micro-units of a currency as an amount a person reads: "12.35 USD", "less than 0.01
 * USD" (usage_money). */
export function usageMoney(micros: unknown, currency: string): string {
  const value = Math.max(0, Math.floor(Number(micros) || 0));
  if (value !== 0 && value < 10_000) return `less than 0.01 ${currency}`;
  const cents = Math.floor((value + 5_000) / 10_000);
  return `${Math.floor(cents / 100)}.${String(cents % 100).padStart(2, "0")} ${currency}`;
}

/** "2026-09-04T00:00:00Z" as "2026-09-04 00:00 UTC" (usage_hour). */
export function usageHour(hour: string): string {
  return hour.length >= 16 && hour[10] === "T" ? `${hour.slice(0, 10)} ${hour.slice(11, 16)} UTC` : hour;
}

export default function MeteringPage() {
  const { viewer, space } = useSession();
  const asked = useParam("customer_id");
  // With no space named, a person sees their current space; the operator chooses one.
  const customerId = asked || (viewer.administrator ? "" : (space?.id ?? viewer.customer_id ?? ""));
  const usage = useRead<Row>(customerId ? "metering_usage" : null, { customer_id: customerId });
  const submit = useQuerySubmit("/metering");
  useMetaTrail(customerId || null, "Usage");
  return (
    <Page eyebrow="Storage and compute" title="Usage" sub="What this space stores and the compute it has used, measured from its repositories, container images and workspaces.">
      <form className="filters" onSubmit={submit} key={customerId}>
        <label>
          Space ID
          <input name="customer_id" defaultValue={customerId} required />
        </label>
        <button type="submit">Show usage</button>
      </form>
      {!customerId ? (
        <PanelEmpty>Choose a space.</PanelEmpty>
      ) : usage.isError ? (
        <ErrorBlock error={usage.error} retry={() => void usage.refetch()} what="usage" />
      ) : !usage.data ? (
        <Skeleton lines={6} />
      ) : (
        <Usage customerId={customerId} usage={usage.data} />
      )}
    </Page>
  );
}

function Usage({ customerId, usage }: { customerId: string; usage: Row }) {
  const summary: Row = usage.summary ?? {};
  const storage: Row = summary.storage ?? {};
  const compute: Row = summary.compute ?? {};
  const quota = Number(usage.quota_bytes ?? 0);
  const priced = !!summary.priced;
  const currency: string = usage.currency ?? "USD";
  const workspaces: Row[] = summary.workspaces ?? [];
  const notMeasured: unknown[] = summary.not_measured ?? [];
  const items: Row[] = usage.items ?? [];
  return (
    <>
      {usage.over_quota && <p className="notice">{usage.warning}</p>}
      <Panel title="Storage">
        <dl>
          <dt>Repositories</dt>
          <dd>{bytes(storage.repositories_bytes ?? 0)}</dd>
          <dt>Container images</dt>
          <dd>{typeof storage.images_bytes === "number" ? bytes(storage.images_bytes) : "Not measured"}</dd>
          <dt>Workspace data</dt>
          <dd>{bytes(storage.workspace_data_bytes ?? 0)}</dd>
          <dt>Total</dt>
          <dd>
            <strong>{bytes(storage.total_bytes ?? 0)}</strong>
          </dd>
          <dt>Repository storage limit</dt>
          <dd>{quota === 0 ? "No limit" : bytes(quota)}</dd>
        </dl>
      </Panel>
      <Panel title="Compute">
        <dl>
          <dt>Repository builds</dt>
          <dd>{usageDuration(compute.repository_build_seconds)}</dd>
          <dt>Image builds</dt>
          <dd>
            {usageDuration(compute.image_build_seconds)} across {compute.image_builds ?? 0} builds, including time queued
          </dd>
          <dt>Workspace runs, training included</dt>
          <dd>
            {usageDuration(compute.workspace_run_seconds)} across {compute.workspace_runs ?? 0} runs
          </dd>
          <dt>Total</dt>
          <dd>
            <strong>{usageDuration(compute.total_seconds)}</strong>
          </dd>
        </dl>
      </Panel>
      {workspaces.length > 0 && (
        <Panel title="Workspaces">
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Workspace</th>
                  <th>Data</th>
                  <th>Datasets</th>
                  <th>Run Time</th>
                  <th>Runs</th>
                </tr>
              </thead>
              <tbody>
                {workspaces.map((workspace, index) =>
                  workspace.measured ? (
                    <tr key={index}>
                      <td>{workspace.name}</td>
                      <td>{bytes(workspace.data_bytes ?? 0)}</td>
                      <td>{workspace.datasets ?? 0}</td>
                      <td>{usageDuration(workspace.run_seconds)}</td>
                      <td>{workspace.runs ?? 0}</td>
                    </tr>
                  ) : (
                    <tr key={index}>
                      <td>{workspace.name}</td>
                      <td colSpan={4}>Not measured: {workspace.message}</td>
                    </tr>
                  ),
                )}
              </tbody>
            </table>
          </div>
        </Panel>
      )}
      {notMeasured.length > 0 && (
        <Panel title="Not Measured Yet">
          <ul>
            {notMeasured.map((line, index) => (
              <li key={index}>{typeof line === "string" ? line : ""}</li>
            ))}
          </ul>
        </Panel>
      )}
      <Panel title="Estimated Charge">
        {priced ? (
          <>
            <p>
              <strong>{usageMoney(usage.cost_micros, currency)}</strong>
            </p>
            <p className="sub">Repository transfer and repository build time at this lake's prices. Storage, images, image builds and workspaces are not priced.</p>
          </>
        ) : (
          <p>This lake has no prices set, so there is no charge to show.</p>
        )}
      </Panel>
      <StorageLimit customerId={customerId} quota={quota} />
      <h2>Repository Usage by Hour</h2>
      {items.length === 0 ? (
        <PanelEmpty>No hourly usage.</PanelEmpty>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Hour</th>
                <th>Uploaded</th>
                <th>Downloaded</th>
                <th>Build Time</th>
                {priced && <th>Charge</th>}
              </tr>
            </thead>
            <tbody>
              {items.map((row) => (
                <tr key={row.hour}>
                  <td>
                    <time dateTime={row.hour}>{usageHour(row.hour ?? "")}</time>
                  </td>
                  <td>{bytes(row.transfer_in_bytes ?? 0)}</td>
                  <td>{bytes(row.transfer_out_bytes ?? 0)}</td>
                  <td>{usageDuration(row.compute_seconds)}</td>
                  {priced && <td>{usageMoney(row.cost_micros, currency)}</td>}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </>
  );
}

function StorageLimit({ customerId, quota }: { customerId: string; quota: number }) {
  const toast = useToast();
  const [value, setValue] = useState(String(quota));
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    setValue(String(quota));
  }, [quota]);
  const submit = async () => {
    setError(null);
    // serve_metering_quota parsed the field as an unsigned integer before calling the capability.
    if (!/^\d+$/.test(value.trim()) || !Number.isSafeInteger(Number(value))) {
      setError(new ApiError(400, "invalid_input", "quota bytes must be a non-negative integer", "/quota_bytes"));
      return;
    }
    setBusy(true);
    try {
      await act("set_storage_quota", { customer_id: customerId, quota_bytes: Number(value) });
      toast("Limit set");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const quotaError = fieldError(error, "quota_bytes");
  return (
    <div className="panel">
      <h2>Repository Storage Limit</h2>
      <p className="sub">New repository writes are refused above this limit; existing content stays readable.</p>
      <Form onSubmit={submit} error={quotaError ? null : error}>
        <Field label="Limit in bytes (0 for no limit)" required error={quotaError}>
          <input type="number" name="quota_bytes" min={0} value={value} onChange={(e) => setValue(e.target.value)} />
        </Field>
        <div>
          <Button variant="primary" type="submit" loading={busy}>
            Set limit
          </Button>
        </div>
      </Form>
    </div>
  );
}
