# Billing

How billing works in Lake. Billing is off unless `LAKE_BILLING` turns it on; usage tracking is on
everywhere.

- **Usage only.** One price list with a rate per usage dimension (and per GPU or model class). A
  bill is the month's usage times the rate, per dimension; there are no plans and no included
  amounts.
- **Prices are configuration.** An instance publishes its own list with `set_price_list`; lake
  ships no prices of its own.
- **Prepaid accounts.** A billing Lake keeps a balance per account, topped up through the payment
  provider, and stops paid work past the overdraft limit.

The price list's API, the bills, invoices, balance and payment providers are in
[`../api/billing.md`](../api/billing.md); usage metering is in [`../api/metering.md`](../api/metering.md);
the Billing pages are in [`../ui/billing.md`](../ui/billing.md).
