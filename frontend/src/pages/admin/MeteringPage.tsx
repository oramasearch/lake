// SPDX-License-Identifier: MIT

/** Usage (`/metering?customer_id=`): what a customer stores and the compute it used, its estimated
 * charge and its repository storage limit (web_ui_metering, serve_metering_page,
 * serve_metering_quota). Figures first; the limit is set in a dialog. */
import { type ReactNode, useEffect, useState } from "react";
import { ArrowDownToLine, ArrowUpFromLine, Gauge, Hammer, HardDrive, Play, Receipt } from "lucide-react";
import { act, ApiError, useRead, type Row } from "@/lib/api";
import { bytes, plural } from "@/lib/format";
import { Modal } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, DataTable, Empty, ErrorBlock, Field, Form, KeyValue, Meter, Page, Skeleton, Split, Stat, StatRow, fieldError } from "@/lib/ui";
import { PanelEmpty, useMetaTrail, useParam, useQuerySubmit } from "@/pages/organizations/shared";
import { usageDuration, usageHour, usageMoney } from "./usageFormat";

export { usageDuration, usageHour, usageMoney };

export default function MeteringPage() {
  const { viewer, space } = useSession();
  const asked = useParam("customer_id");
  // With no customer named, a person sees their current one; the operator chooses one.
  const customerId = asked || (viewer.administrator ? "" : (space?.id ?? viewer.customer_id ?? ""));
  const usage = useRead<Row>(customerId ? "metering_usage" : null, { customer_id: customerId });
  const submit = useQuerySubmit("/metering");
  const [limitOpen, setLimitOpen] = useState(false);
  useMetaTrail(customerId || null, "Usage");
  return (
    <Page
      eyebrow="Organization"
      title="Usage"
      sub={
        viewer.administrator
          ? "What a customer stores and the compute it has used, measured from its repositories, container images and workspaces."
          : "What you store and the compute you have used, measured from your repositories, container images and workspaces."
      }
      actions={
        // The storage quota is the operator's or the instance administrator's alone (lake#976).
        usage.data && viewer.administrator ? (
          <Button variant="primary" onClick={() => setLimitOpen(true)}>
            <Gauge size={14} aria-hidden /> Set storage limit
          </Button>
        ) : undefined
      }
    >
      <form className="mb-4 flex flex-wrap items-end gap-2" onSubmit={submit} key={customerId}>
        <label className="field m-0 min-w-0 flex-1 basis-[16rem] sm:max-w-[26rem]">
          <span className="field-label">Customer ID</span>
          <input name="customer_id" defaultValue={customerId} required />
        </label>
        <Button type="submit">Show usage</Button>
      </form>
      {!customerId ? (
        <Empty icon={HardDrive} title="Choose a customer">
          Enter a customer's ID to see what it stores and the compute it has used.
        </Empty>
      ) : usage.isError ? (
        <ErrorBlock error={usage.error} retry={() => void usage.refetch()} what="usage" />
      ) : !usage.data ? (
        <Skeleton lines={6} />
      ) : (
        <Usage usage={usage.data} onSetLimit={() => setLimitOpen(true)} />
      )}
      {limitOpen && usage.data && <StorageLimit customerId={customerId} quota={Number(usage.data.quota_bytes ?? 0)} onClose={() => setLimitOpen(false)} />}
    </Page>
  );
}

function Usage({ usage, onSetLimit }: { usage: Row; onSetLimit: () => void }) {
  const summary: Row = usage.summary ?? {};
  const storage: Row = summary.storage ?? {};
  const compute: Row = summary.compute ?? {};
  const quota = Number(usage.quota_bytes ?? 0);
  const priced = !!summary.priced;
  const currency: string = usage.currency ?? "USD";
  const workspaces: Row[] = summary.workspaces ?? [];
  const notMeasured: unknown[] = summary.not_measured ?? [];
  const items: Row[] = usage.items ?? [];
  const total = Number(storage.total_bytes ?? 0);
  const repositories = Number(storage.repositories_bytes ?? 0);
  const imagesMeasured = typeof storage.images_bytes === "number";
  const uploaded = items.reduce((s, r) => s + Number(r.transfer_in_bytes ?? 0), 0);
  const downloaded = items.reduce((s, r) => s + Number(r.transfer_out_bytes ?? 0), 0);
  const buildSeconds = Number(compute.repository_build_seconds ?? 0) + Number(compute.image_build_seconds ?? 0);
  const share = (value: number) => (total > 0 ? `${Math.round((value / total) * 100)}%` : "0%");
  return (
    <>
      {usage.over_quota && <Banner tone="warn">{usage.warning}</Banner>}
      <StatRow>
        <Stat label="Stored" value={bytes(total)} icon={HardDrive} sub={quota === 0 ? "no repository storage limit" : `repositories ${bytes(repositories)} of ${bytes(quota)}`} tone={usage.over_quota ? "bad" : undefined} />
        <Stat label="Transfer" value={bytes(uploaded + downloaded)} icon={ArrowUpFromLine} sub={`${bytes(uploaded)} up · ${bytes(downloaded)} down, by the hours below`} />
        <Stat label="Builds" value={usageDuration(buildSeconds)} icon={Hammer} sub={`${plural(Number(compute.image_builds ?? 0), "image build")}, repository builds included`} />
        <Stat label="Run time" value={usageDuration(compute.workspace_run_seconds)} icon={Play} sub={`${plural(Number(compute.workspace_runs ?? 0), "workspace run")}, training included`} />
      </StatRow>
      <Split
        side={
          <>
            <Card
              label="Repository storage limit"
              tone={usage.over_quota ? "exception" : "default"}
              actions={
                <Button variant="ghost" onClick={onSetLimit}>
                  Change
                </Button>
              }
            >
              <div className="font-mono text-[20px] font-semibold text-ink">{quota === 0 ? "No limit" : bytes(quota)}</div>
              {quota > 0 && (
                <div className="mt-2">
                  <Meter value={repositories} max={quota} label={`${bytes(repositories)} used`} tone={repositories > quota ? "bad" : undefined} />
                </div>
              )}
              <p className="mb-0 text-[12.5px] text-ink-3">New repository writes are refused above this limit; existing content stays readable.</p>
            </Card>
            <Card label="Estimated charge">
              {priced ? (
                <>
                  <div className="flex items-center gap-2 font-mono text-[20px] font-semibold text-ink">
                    <Receipt size={16} className="text-ink-3" aria-hidden />
                    {usageMoney(usage.cost_micros, currency)}
                  </div>
                  <p className="mb-0 text-[12.5px] text-ink-3">Repository transfer and repository build time at this Lake's prices. Storage, images, image builds and workspaces are not priced.</p>
                </>
              ) : (
                <p className="m-0 text-[13px] text-ink-2">This Lake has no prices set, so there is no charge to show.</p>
              )}
            </Card>
          </>
        }
      >
        <Card label="Storage" title={bytes(total)}>
          <div className="space-y-3">
            <MeterRow label="Repositories" value={bytes(repositories)} meter={<Meter value={repositories} max={total || 1} label={share(repositories)} />} />
            <MeterRow
              label="Container images"
              value={imagesMeasured ? bytes(storage.images_bytes) : "Not measured"}
              meter={imagesMeasured ? <Meter value={Number(storage.images_bytes)} max={total || 1} label={share(Number(storage.images_bytes))} /> : null}
            />
            <MeterRow
              label="Workspace data"
              value={bytes(storage.workspace_data_bytes ?? 0)}
              meter={<Meter value={Number(storage.workspace_data_bytes ?? 0)} max={total || 1} label={share(Number(storage.workspace_data_bytes ?? 0))} />}
            />
          </div>
        </Card>
        <Card label="Compute" title={usageDuration(compute.total_seconds)}>
          <KeyValue
            rows={[
              ["Repository builds", usageDuration(compute.repository_build_seconds)],
              ["Image builds", `${usageDuration(compute.image_build_seconds)} across ${compute.image_builds ?? 0} builds, including time queued`],
              ["Workspace runs, training included", `${usageDuration(compute.workspace_run_seconds)} across ${compute.workspace_runs ?? 0} runs`],
              ["Total", <strong>{usageDuration(compute.total_seconds)}</strong>],
            ]}
          />
        </Card>
      </Split>
      {notMeasured.length > 0 && (
        <div className="mt-4">
          <Banner tone="quiet" title="Not Measured Yet">
            <ul className="m-0 pl-4">
              {notMeasured.map((line, index) => (
                <li key={index}>{typeof line === "string" ? line : ""}</li>
              ))}
            </ul>
          </Banner>
        </div>
      )}
      {workspaces.length > 0 && (
        <Card label="Workspaces" padded={false} className="mt-4">
          <DataTable
            rows={workspaces}
            rowKey={(w, i) => `${w.name}:${i}`}
            columns={[
              { key: "name", label: "Workspace", sort: (w) => w.name, render: (w) => <span className="font-semibold text-ink">{w.name}</span> },
              {
                key: "data",
                label: "Data",
                numeric: true,
                sort: (w) => Number(w.data_bytes ?? 0),
                render: (w) => (w.measured ? bytes(w.data_bytes ?? 0) : <span className="text-ink-3">Not measured: {w.message}</span>),
              },
              { key: "datasets", label: "Datasets", numeric: true, render: (w) => (w.measured ? <span className="font-mono">{w.datasets ?? 0}</span> : null) },
              { key: "run", label: "Run Time", numeric: true, sort: (w) => Number(w.run_seconds ?? 0), render: (w) => (w.measured ? usageDuration(w.run_seconds) : null) },
              { key: "runs", label: "Runs", numeric: true, render: (w) => (w.measured ? <span className="font-mono">{w.runs ?? 0}</span> : null) },
            ]}
          />
        </Card>
      )}
      <Card label="Repository usage by hour" padded={false} className="mt-4">
        <DataTable
          rows={items}
          rowKey={(r) => r.hour}
          empty={
            <div className="p-4">
              <PanelEmpty>No hourly usage.</PanelEmpty>
            </div>
          }
          columns={[
            {
              key: "hour",
              label: "Hour",
              sort: (r) => r.hour,
              render: (r) => (
                <time dateTime={r.hour} className="whitespace-nowrap">
                  {usageHour(r.hour ?? "")}
                </time>
              ),
            },
            {
              key: "in",
              label: "Uploaded",
              numeric: true,
              sort: (r) => Number(r.transfer_in_bytes ?? 0),
              render: (r) => (
                <span className="inline-flex items-center gap-1">
                  <ArrowUpFromLine size={12} className="text-ink-3" aria-hidden />
                  {bytes(r.transfer_in_bytes ?? 0)}
                </span>
              ),
            },
            {
              key: "out",
              label: "Downloaded",
              numeric: true,
              sort: (r) => Number(r.transfer_out_bytes ?? 0),
              render: (r) => (
                <span className="inline-flex items-center gap-1">
                  <ArrowDownToLine size={12} className="text-ink-3" aria-hidden />
                  {bytes(r.transfer_out_bytes ?? 0)}
                </span>
              ),
            },
            { key: "compute", label: "Build Time", numeric: true, sort: (r) => Number(r.compute_seconds ?? 0), render: (r) => usageDuration(r.compute_seconds) },
            ...(priced ? [{ key: "cost", label: "Charge", numeric: true, render: (r: Row) => usageMoney(r.cost_micros, currency) }] : []),
          ]}
        />
      </Card>
    </>
  );
}

function MeterRow({ label, value, meter }: { label: string; value: string; meter: ReactNode }) {
  return (
    <div>
      <div className="mb-1 flex items-baseline justify-between gap-3 text-[13px]">
        <span className="text-ink-2">{label}</span>
        <span className="font-mono text-ink">{value}</span>
      </div>
      {meter}
    </div>
  );
}

function StorageLimit({ customerId, quota, onClose }: { customerId: string; quota: number; onClose: () => void }) {
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
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const quotaError = fieldError(error, "quota_bytes");
  const parsed = /^\d+$/.test(value.trim()) ? Number(value) : null;
  return (
    <Modal
      title="Repository Storage Limit"
      subtitle="New repository writes are refused above this limit; existing content stays readable."
      onClose={onClose}
      width={480}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="storage-limit" loading={busy}>
            Set limit
          </Button>
        </>
      }
    >
      <Form id="storage-limit" onSubmit={submit} error={quotaError ? null : error}>
        <Field label="Limit in bytes (0 for no limit)" required error={quotaError} help={parsed === null ? undefined : parsed === 0 ? "No limit." : `${bytes(parsed)}.`}>
          <input type="number" name="quota_bytes" min={0} value={value} onChange={(e) => setValue(e.target.value)} autoFocus />
        </Field>
      </Form>
    </Modal>
  );
}
