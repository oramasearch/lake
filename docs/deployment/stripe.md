# Runbook: set up Stripe payments

Lake bills accounts and collects payment through Stripe (decision 57, lake#620). Lake issues each
invoice; Stripe hosts the payment page and tells Lake, through a signed webhook, when an invoice is
paid, fails or is voided. The API side is [`../api/billing.md`](../api/billing.md#stripe); this page
is the deployment procedure.

**Who runs it:** an Orama operator with Stripe dashboard access and `kubectl` on the cluster (or
Railway access for a Railway deployment). It is done once per Lake deployment, and again to rotate a
key.

## Before you start

- **Billing is on.** `GET https://<lake host>/v1/server_info` answers `"billing": true`. If not, set
  `LAKE_BILLING=on` on the deployment first ([aks.md](aks.md)).
- **The schema has `0074_payment_provider`** (guard `payment_provider_guard`) or later. It does on
  every Lake built after 2026-10-11.
- **The account is shared** with Orama Cloud v1 and, in live mode, with the other production Lake
  (lake#855, [../api/billing.md](../api/billing.md#stripe)). Lake tags every customer, invoice and
  Checkout Session it creates with `metadata[orama_app]=lake` and `metadata[lake_instance]`, its
  `LAKE_PUBLIC_HOST`, and answers events about other apps' and other Lakes' objects `200
  {"received": true, "ignored": true}` without recording them. So `LAKE_PUBLIC_HOST` must be set,
  different on every Lake that shares the account, and kept: a Lake whose host changes takes the
  objects it tagged before for another Lake's.
- **Lake's products are in the account.** Every invoice line names the product `lake_usage`, and an
  order sheet Checkout `lake_private_warehouse` or `lake_dedicated_cluster`
  ([../api/billing.md](../api/billing.md#stripe)). The operator creates them in each account from
  the Lake's `spec.payments` (orama-operator `docs/lake/payments.md`); never create them by hand.
  Without them every invoice fails with "Stripe has no product lake_usage in this account; the
  operator creates it (spec.payments)".
- **Use test mode first.** Do the whole runbook with Stripe's test keys (`sk_test_…`) and a test
  card; switch to live keys only after the check passes.
- **The keys are Orama's, never a customer's.** They are platform environment variables: a
  Kubernetes Secret referenced from `spec.env` on the cluster, or Railway variables on a Railway
  deployment. Never put them in a user's secret store, a secret mount, a file or a commit
  ([secrets.md](secrets.md), lake#628).

| Lake | Host | Where the keys go |
|---|---|---|
| Development (AKS, `Lake/prod`) | `lake.oramadev.net` | Secret `lake-prod-stripe` in namespace `lake`, already referenced by `spec.env` with blank values |
| Orama Cloud (Railway) | `lake.live` | Railway variables `LAKE_STRIPE_SECRET_KEY` and `LAKE_STRIPE_WEBHOOK_SECRET` on the Lake service |

## 1. Create the keys in Stripe

1. In the Stripe dashboard (test mode), **Developers → API keys**: take the **secret key**
   (`sk_test_…`). A **restricted key** (`rk_test_…`) is better: give it write access to
   **Customers**, **Invoices**, **Invoice Items** and **Checkout Sessions** (the order sheet), and
   nothing else.
2. **Developers → Webhooks → Add endpoint:**
   - URL: `https://<lake host>/billing/stripe/webhook` — on dev
     `https://lake.oramadev.net/billing/stripe/webhook`, on Orama Cloud
     `https://lake.live/billing/stripe/webhook`.
   - Events: `invoice.paid`, `invoice.payment_failed`, `invoice.voided`, and for the order sheet's
     card payments (lake#831, [../api/orders.md](../api/orders.md#paying)):
     `checkout.session.completed`, `checkout.session.async_payment_succeeded`,
     `checkout.session.async_payment_failed`, `checkout.session.expired`.
   - Save, then reveal the endpoint's **signing secret** (`whsec_…`).

Keep both values in your password manager; nothing else needs them.

## 2. Give Lake the keys

### Development (AKS)

The Secret already exists with blank values, so replace its contents. Read each value without
echoing it, so it stays out of shell history:

```sh
read -rs STRIPE_SECRET_KEY; read -rs STRIPE_WEBHOOK_SECRET
kubectl --context orama-production-cluster-01 -n lake create secret generic lake-prod-stripe \
  --from-literal=secret_key="$STRIPE_SECRET_KEY" --from-literal=webhook_secret="$STRIPE_WEBHOOK_SECRET" \
  --dry-run=client -o yaml | kubectl --context orama-production-cluster-01 apply -f -
unset STRIPE_SECRET_KEY STRIPE_WEBHOOK_SECRET
kubectl --context orama-production-cluster-01 -n lake rollout restart deployment/lake-prod
kubectl --context orama-production-cluster-01 -n lake rollout status deployment/lake-prod
```

Lake reads its environment only at start, hence the restart. `Lake/prod` already carries the two
`spec.env` entries (`valueFrom.secretKeyRef` to `lake-prod-stripe`, keys `secret_key` and
`webhook_secret`); on a Lake without them, add them as in [aks.md](aks.md#stripe).

### Orama Cloud (Railway)

In the Lake service's **Variables**, set `LAKE_STRIPE_SECRET_KEY` and `LAKE_STRIPE_WEBHOOK_SECRET`
(sealed), then redeploy. Do not paste them into a shell or a chat.

## 3. Check

1. `GET https://<lake host>/v1/server_info` answers `"payment_provider": "stripe"`. If it still says
   `manual`, the reason is on the Billing page and in `billing_account`: a key missing, or not
   `sk_`/`rk_` (secret key) or `whsec_` (webhook secret).
2. Signed in as an operator, open **Administration → Billing**: the Payment figure says **Stripe**.
3. Issue a test invoice for a test account (Administration → Billing, or `issue_invoice`), open it,
   choose **Pay with Stripe**, and pay with Stripe's test card `4242 4242 4242 4242` (any future
   date, any CVC). When the test account has a billing contact, Lake also asks Stripe to email the
   invoice: **Developers → Logs** shows `POST /v1/invoices/<id>/send` answered `200`. Stripe sends
   no email in test mode or a sandbox, so this proves the request, not delivery
   ([billing.md](../api/billing.md#emailing-invoices)).
4. Within a minute the invoice in Lake reads **Paid**. In the Stripe dashboard, **Webhooks → the
   endpoint → Event deliveries** shows `invoice.paid` delivered with Lake's `200`.
5. In the Stripe dashboard, the invoice's and its customer's **Metadata** show `orama_app` `lake`
   and `lake_instance` this Lake's host, the invoice is described "Lake usage, `<month>`", and each
   of its lines is of the product `lake_usage`.

If the invoice stays open: a delivery answered `400` means a wrong webhook secret (re-check step 1's
`whsec_…` and step 2); `404` means Lake has no Stripe keys (step 2 not applied or Lake not
restarted); `200` with `"ignored": true` means the invoice's `lake_instance` is not this Lake's
`LAKE_PUBLIC_HOST`. Stripe retries failed deliveries for three days, so a fixed key catches up.
If issuing fails with "Stripe has no product", the account lacks Lake's products: apply the Lake's
`spec.payments` and issue again, and again after 24 hours if Stripe repeats the answer it saved.

## 4. Go live

Repeat steps 1–3 with Stripe in **live mode**: a live restricted key (`rk_live_…`), a live webhook
endpoint with the same three events, and its live signing secret. Invoices issued while on test keys
stay in Stripe's test data: settle or void the open ones first, because Lake cannot learn about a
payment made in test mode afterwards. Each account's test-mode customer is replaced by a live one at
its next invoice (lake#856, [../api/billing.md](../api/billing.md#stripe)). Record the switch (date,
deployment) on lake#620.

## Rotate a key

- **Secret key:** create the new key in Stripe, replace the value in the Secret (or Railway
  variable) as in step 2, restart Lake, check `server_info`, then delete the old key in Stripe.
- **Webhook secret:** in Stripe, **Roll secret** on the endpoint (the old one keeps working for the
  window you choose), set the new value as in step 2, restart Lake within that window, and watch one
  delivery succeed.

## Turn Stripe off

Set both values to empty (or remove the two `spec.env` entries) and restart Lake. Lake falls back to
the manual provider and the Billing page says card payment is not set up. Invoices already issued
through Stripe stay payable there; Stripe's notices are answered `404` until keys return, so Stripe
retries them for three days.
