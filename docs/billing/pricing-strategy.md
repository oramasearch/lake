# Lake pricing strategy

**Status:** 2026-10-11. The price list is published on the development Lake (lake.oramadev.net),
effective October 2026. Card payment is wired (Stripe, decision 57) but its keys are blank there.

Lake is priced by usage only: one rate per resource, no plans, no free tier, no monthly fee and no
seats ([lake#635](https://github.com/oramasearch/lake/issues/635)). Metis is enterprise software,
priced per engagement by Orama's sales team, and is not on Lake's price list. Orama Cloud bills Lake
through Stripe; an enterprise installation tracks the same usage but is never billed.

The rates themselves, with each one's reference price, margin and the worked bills, are in
[`../api/billing.md`](../api/billing.md#price-list). That page is the single source for numbers;
this one says why the prices are what they are and how they are governed.

## Principles

Prices follow what the resource costs on the open market, so they are easy to defend and easy to
revisit.

1. **Pay for what you use.** An account pays for what its workspaces use: compute (CPU, RAM, GPU and
   GPU memory, [lake#643](https://github.com/oramasearch/lake/issues/643)), storage, transfer,
   volumes, model tokens, speech to text, API and MCP calls, predictions and index searches. Every
   kind of usage is metered and billed ([lake#658](https://github.com/oramasearch/lake/issues/658));
   a workload that runs is never free.
2. **Cost-plus on public list rates.** Each rate is a public on-demand list price for the same
   resource (US regions, read 2026-10-11) times a stated margin: about 1.5–2× for compute, 1.7–2.5×
   for storage and volumes. The margin pays for running it as a managed service: one bill,
   governance, audit and support.
3. **Pass-through at a thin margin.** Model tokens and speech to text are billed at the provider's
   rate plus 20%. Customers can compare them, so a wide margin would not hold.
4. **Billed per minute on what is scheduled.** Time is measured by the second and billed per minute,
   the month's total rounded up once. Compute is billed for what a workload is scheduled with (its
   requests), as serverless containers and managed Kubernetes bill, not for what it consumes.
5. **One charge per resource.** A workspace engine's disk is billed as its provisioned volume only;
   the bytes stored on it are shown but not billed again (decision 61). Speech is billed by the
   minute only, never also as model tokens. Registry packages are the publishing account's storage
   and transfer (decision 62).
6. **Metis is sold, not self-served.** It is a program priced per engagement through sales.
7. **Two kinds of installation.** Orama Cloud is billed. An enterprise installation tracks the same
   usage and shows the same charts, with billing switched off entirely (`LAKE_BILLING`).

## How billing works

Usage is metered in each workspace's own Zetta; billing records live in Lake's Postgres; Stripe only
collects the payment.

1. **Metering.** Every workspace records its usage in its own Zetta (`lake_usage`, `lake_storage`),
   kept 400 days. Usage never leaves the workspace (decision 56). Model calls are counted at Lake's
   model gateway, which agents cannot skip ([`../api/model-gateway.md`](../api/model-gateway.md));
   finished jobs are charged once each from the resources the operator recorded when it created them
   ([`../api/metering.md`](../api/metering.md)).
2. **The bill.** At month end Lake reads each workspace's usage, multiplies it by the price list and
   freezes the quantities on the invoice. It refuses to issue while any workspace's usage cannot be
   read. Issued invoices keep their lines when prices change.
3. **Payment.** Lake creates a matching Stripe invoice. The customer pays on Stripe's hosted page by
   card or bank transfer, and Stripe's signed webhooks mark it paid, failed or voided in Lake
   (decision 57).
4. **Keys.** Orama's Stripe keys are platform environment variables, never a customer's secret store
   ([`../deployment/secrets.md`](../deployment/secrets.md)).

## Risks and gaps

| Risk | Why it matters | Fix |
|---|---|---|
| Model tokens are one blended rate | $9.00 per million assumes 10 input tokens to each output token; output-heavy agents cost Orama more than they bill | Price input and output separately, each at provider cost plus 20% (the gateway already records them apart) |
| Spending is capped for model tokens only | The monthly `model_tokens` limit is enforced at the gateway; other limits only warn, so a runaway workload bills at full rate before anyone acts | Hard spending limits per account, and a card before usage starts (open decision below) |
| Invoices are issued by hand | No job issues each month's invoices yet | [lake#621](https://github.com/oramasearch/lake/issues/621): issue invoices after each month ends |
| App ingress traffic is not yet counted | An app serving heavy traffic is billed for its compute but not its transfer | [lake#661](https://github.com/oramasearch/lake/issues/661) |

Fixed on 2026-10-11: compute was priced per job at an assumed 2 vCPU and 4 GiB; it is now metered as
CPU, RAM, GPU and GPU memory (lake#643), and finished jobs, cron runs and methodology runs are
charged even when their pods are gone (lake#658).

## Governance

- **Review:** every quarter, next on 2027-01-11, or sooner if a reference rate moves more than 15%
  ([`../api/billing.md`](../api/billing.md#revisit) lists the triggers).
- **Where it is recorded:** the rates and their references in
  [`../api/billing.md`](../api/billing.md#price-list); the cited research on
  [lake#623](https://github.com/oramasearch/lake/issues/623) and
  [lake#643](https://github.com/oramasearch/lake/issues/643) (vendors are named only there);
  decisions 56 (billing records in Postgres), 57 (Stripe), 61 (engine disk) and 62 (registry
  packages) in liquid-bridge `docs/decisions.md`.
- **Who publishes:** only the operator or the instance administrator (`set_price_list`,
  [`../api/billing.md`](../api/billing.md#publish-the-price-list)).

## Open decisions

- [x] Publish the price list on the development Lake (2026-10-11, effective October 2026).
- [ ] Set the Stripe keys and webhook so invoices can be paid by card. The development Lake reads
      them from the Secret `lake-prod-stripe`, which holds blank values today
      ([`../deployment/aks.md`](../deployment/aks.md#stripe)).
- [ ] Decide whether a card is required before any usage.
- [ ] Decide whether committed-use discounts are offered for large accounts.
