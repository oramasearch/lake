/** /webhooks?repository_id= (web_ui_webhooks): each webhook with its signed delivery history; a
 * failed delivery can be retried. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import {
  ForgeBadge,
  ForgePage,
  ForgeTabs,
  forgeAct,
  Identifier,
  itemsOf,
  pending,
  repositoryName,
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
  return (
    <ForgePage eyebrow="repository automation" title="Webhooks" sub={`${repositoryName(r)} · signed delivery history`}>
      <ForgeTabs repository={r} current="webhooks" />
      {hooks.length === 0 && <div className="panel empty">No webhooks are configured.</div>}
      {hooks.map((webhook) => {
        const attempts = itemsOf(deliveries.data).filter((delivery) => delivery.value.webhook_id === webhook.id);
        return (
          <section key={webhook.id} className="panel webhook-card">
            <header>
              <div>
                <h2>{webhook.value.url ?? ""}</h2>
                <p className="sub">{dateTime(webhook.created_at)}</p>
              </div>
              <Identifier value={webhook.id} />
            </header>
            <p>Events: {(webhook.value.events ?? []).join(", ")}</p>
            <h3>Deliveries</h3>
            {attempts.map((delivery) => {
              const delivered = !!delivery.value.delivered;
              return (
                <article key={delivery.id} className="delivery-row">
                  <div>
                    <strong>{delivery.value.event ?? "event"}</strong>
                    <ForgeBadge value={delivered ? "delivered" : "failed"} />
                    <p className="sub">
                      attempt {delivery.value.attempt ?? 0} · status {delivery.value.status ?? 0} · {delivery.value.error || "no error"}
                    </p>
                  </div>
                  <Identifier value={delivery.id} />
                  {!delivered && viewer.signed_in && <Retry repositoryId={id} deliveryId={delivery.id} />}
                </article>
              );
            })}
            {attempts.length === 0 && <div className="empty compact-empty">No delivery attempts.</div>}
          </section>
        );
      })}
    </ForgePage>
  );
}

/** Retry a failed delivery (serve_webhook_retry); every attempt is kept. */
function Retry({ repositoryId, deliveryId }: { repositoryId: string; deliveryId: string }) {
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <span>
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
        <span className="error" role="alert">
          {error}
        </span>
      )}
    </span>
  );
}
