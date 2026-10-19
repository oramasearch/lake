# Runbook: set up Resend email

Lake sends the order sheet's email through **Resend** (lake#831): an order form to support, a paid
order and a failed one to support, and the customer's copy and status updates
([`../api/orders.md`](../api/orders.md#email)). **Resend is a deployment dependency of every Lake
that serves the order sheet** (the apex `lake.live` picker): without a key, orders are still
recorded and the page still says an order was received, but **no email is sent**, so support never
hears about an order form. This page is the deployment procedure.

**Who runs it:** an Orama operator with access to Orama's Resend account, the Cloudflare zone of the
sending domain, and the operator's `config/clusters/` (Secrets on the managing cluster are created
by Steve). It is done once per environment, and again to rotate the key.

## What Lake reads

| Variable | What | Default |
|---|---|---|
| `LAKE_RESEND_API_KEY` | Orama's Resend API key (`re_…`). A platform secret: a Kubernetes Secret referenced from the Lake's `spec.env` with `valueFrom.secretKeyRef`, never a literal, never a workspace secret ([secrets.md](secrets.md)) | unset: email is off |
| `LAKE_RESEND_FROM` | The sender. Its domain must be verified in Resend | `Lake <orders@lake.live>` |
| `LAKE_SUPPORT_EMAIL` | Where order forms, paid orders and failures go | `support@orama.com` |

Lake reads them only at start. The startup log line carries `resend_api_key_set` (`yes`/`no`).

## Before you start

- **The Lake runs lake with the order sheet** (lake#833 or later) and serves the apex picker
  (`apexHost`). Only that member sends order email, but every member may carry the key.
- **One sending domain per environment.** Production sends from `lake.live`. Nonprod (`lake-dev`,
  `lake-testing`, `lake-staging`) sends from an `oramadev.net` address, set with `LAKE_RESEND_FROM`,
  and should send to a test inbox through `LAKE_SUPPORT_EMAIL`, not to `support@orama.com`.
- **The key is Orama's, never a customer's.** Never put it in a user's secret store, a file, a
  commit or a chat.

## 1. Verify the sending domain in Resend

1. In Resend, **Domains → Add domain**: `lake.live` (production) or `oramadev.net` (nonprod).
2. Resend lists the DNS records it needs (SPF `TXT`, the DKIM `TXT`/`CNAME` records, and an `MX`
   on the bounce subdomain). Add them in the Cloudflare zone **unproxied**. These are mail records,
   not a Lake's own records, so the operator does not own them; note them on the deployment's issue.
3. Wait for **Verified**. Nothing is delivered from an unverified domain.

## 2. Create the key

**API Keys → Create API key**: permission **Sending access**, restricted to the verified domain.
One key per environment (`lake-prod`, `lake-nonprod`), so one can be revoked alone. Keep it in the
password manager.

## 3. Give Lake the key

Change happens through the operator's committed set files, never by hand on a cluster (umbrella
`CLAUDE.md`, *Never change a cluster by hand*).

1. **The Secret** (Steve creates it on the managing cluster, namespace `lake`): `lake-prod-resend`
   for production, `lake-nonprod-resend` for nonprod, key `api_key`. Read the value without echoing
   it:

   ```sh
   read -rs RESEND_API_KEY
   kubectl --context orama-production-cluster-01 -n lake create secret generic lake-prod-resend \
     --from-literal=api_key="$RESEND_API_KEY"
   unset RESEND_API_KEY
   ```

2. **The set file** (`orama-operator` `config/clusters/sets/lake-prod.yaml` or `lake-nonprod.yaml`,
   by pull request into `dev`): list the Secret under the set's `secrets:` so it is copied into each
   member's namespace, and add to each member's `env`:

   ```yaml
   - name: LAKE_RESEND_API_KEY
     valueFrom: {secretKeyRef: {name: lake-prod-resend, key: api_key}}
   # nonprod only:
   - name: LAKE_RESEND_FROM
     value: Lake <orders@oramadev.net>
   - name: LAKE_SUPPORT_EMAIL
     value: <test inbox>
   ```

   A member's `env` list replaces the template's whole, so add the entries to every member that has
   its own `env`.

3. Dry-run, apply the file with Steve's consent, wait for the set to be Ready, then merge the PR
   ([`lake-devops`](https://github.com/oramasearch/orama-bridge/blob/main/.claude/skills/lake-devops/SKILL.md)).
   Follow the pipeline: dev, testing, staging, then prod.

## 4. Check

1. The Lake's log at start says `resend_api_key_set` `yes`, and no `cloud_order_email_disabled` line
   follows.
2. On the environment's order sheet, send an **order form** (any offer, "Contact us") with a test
   inbox as the contact email.
3. Within a minute: support's inbox (`LAKE_SUPPORT_EMAIL`) has the order with every field and the
   order id, and the test inbox has the copy with the status link. Resend's **Emails** page shows
   both as delivered.
4. If not: the order's `email_error` says why (`LAKE_RESEND_API_KEY is not set`, or Resend's answer
   to a 401/403 or an unverified domain). Delivery is retried after 1, 2, 4 … minutes, at most an
   hour apart, so a fixed key catches up without a new order.

Record the result (environment, date, image) on the deployment's issue.

## Rotate the key

Create the new key in Resend, replace the Secret's value (Steve), and once the set has copied it,
restart the Lake so it reads the new value (lake reads its environment only at start; a restart on a
cluster needs Steve's consent). Check step 4's log line, then delete the old key in Resend.

## Turn email off

Remove `LAKE_RESEND_API_KEY` from the set file and apply it. Orders keep being recorded and their
emails stay due; they go out when a key is set again.
