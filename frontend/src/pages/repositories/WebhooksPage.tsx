// SPDX-License-Identifier: MIT

/** /webhooks?repository_id= (web_ui_webhooks): each webhook with its signed delivery history; a
 * failed delivery can be retried. Figures on top: webhooks, deliveries and failures. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { CircleAlert, Send, Webhook } from "lucide-react";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, Card, Chip, Empty, Stat, StatRow } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import {
  ForgePage,
  ForgeStatus,
  forgeAct,
  Identifier,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";

export default function WebhooksPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Webhooks id={id} />;
}

function Webhooks({ id }: { id: string }) {
  const { viewer } = useSession();
  const repository = useForgeRead<Repository>("get_repository", { id });
  const webhooks = useForgeRead<Items<ForgeRecord>>("list_webhooks", { repository_id: id, limit: 1000 });
  const deliveries = useForgeRead<Items<ForgeRecord>>("list_webhook_deliveries", { repository_id: id, limit: 1000 });
  useRepositoryTrail(repository.data, "Webhooks");
  const wait = pending(repository, webhooks, deliveries);
  if (wait) return wait;
  const r = repository.data!;
  const hooks = itemsOf(webhooks.data);
  const attemptsAll = itemsOf(deliveries.data);
  const failed = attemptsAll.filter((delivery) => !delivery.value.delivered).length;
  return (
    <ForgePage title="Webhooks" repository={r} tab="webhooks" sub="Each webhook and its signed delivery history.">
      {hooks.length === 0 ? (
        <Empty icon={Webhook} title="No webhooks yet">
          No webhooks are configured.
        </Empty>
      ) : (
        <>
          <StatRow cols={3}>
            <Stat label="Webhooks" value={hooks.length} icon={Webhook} sub="configured" />
            <Stat label="Deliveries" value={attemptsAll.length} icon={Send} sub="attempts kept" />
            <Stat label="Failed" value={failed} icon={CircleAlert} tone={failed ? "bad" : undefined} sub={failed ? "can be retried" : "none"} />
          </StatRow>
          <div className="space-y-4">
            {hooks.map((webhook) => {
              const attempts = attemptsAll.filter((delivery) => delivery.value.webhook_id === webhook.id);
              return (
                <Card
                  key={webhook.id}
                  padded={false}
                  label={<Identifier value={webhook.id} />}
                  title={<span className="break-all font-mono text-[13.5px]">{webhook.value.url ?? ""}</span>}
                  actions={<span className="text-[12px] text-ink-3">{dateTime(webhook.created_at)}</span>}
                >
                  <div className="flex flex-wrap items-center gap-1.5 border-b border-hairline px-4 py-2.5">
                    <span className="label m-0 mr-1">Events</span>
                    {(webhook.value.events ?? []).map((event: string) => (
                      <Chip key={event}>{event}</Chip>
                    ))}
                  </div>
                  {attempts.length === 0 ? (
                    <p className="muted m-0 px-4 py-3">No delivery attempts.</p>
                  ) : (
                    <ul className="m-0 list-none divide-y divide-hairline p-0">
                      {attempts.map((delivery) => {
                        const delivered = !!delivery.value.delivered;
                        return (
                          <li key={delivery.id} className="flex flex-wrap items-center gap-3 px-4 py-2.5">
                            <ForgeStatus value={delivered ? "delivered" : "failed"} />
                            <div className="min-w-0 flex-1">
                              <strong>{delivery.value.event ?? "event"}</strong>
                              <div className="text-[12px] text-ink-3">
                                attempt {delivery.value.attempt ?? 0} · status {delivery.value.status ?? 0} · {delivery.value.error || "no error"}
                              </div>
                            </div>
                            <Identifier value={delivery.id} />
                            {!delivered && viewer.signed_in && <Retry repositoryId={id} deliveryId={delivery.id} />}
                          </li>
                        );
                      })}
                    </ul>
                  )}
                </Card>
              );
            })}
          </div>
        </>
      )}
    </ForgePage>
  );
}

/** Retry a failed delivery (serve_webhook_retry); every attempt is kept. */
function Retry({ repositoryId, deliveryId }: { repositoryId: string; deliveryId: string }) {
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <span className="flex items-center gap-2">
      <Button
        loading={busy}
        onClick={async () => {
          setBusy(true);
          setError(null);
          try {
            await forgeAct("retry_webhook_delivery", { repository_id: repositoryId, delivery_id: deliveryId });
            toast("Delivery retried");
          } catch (e) {
            setError((e as Error).message);
          } finally {
            setBusy(false);
          }
        }}
      >
        Retry
      </Button>
      {error && (
        <span className="text-[12px] text-bad" role="alert">
          {error}
        </span>
      )}
    </span>
  );
}
