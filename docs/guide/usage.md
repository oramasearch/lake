# Usage and Metering

**Organization → Usage** shows what an account stores and the compute it has used: its
repositories, container images and workspace data, its builds and its workspace runs (training
included). It also shows the repository storage limit and, when the lake has prices set, an
estimated charge.

## Concepts

| Term | What it is |
|---|---|
| **Stored** | The bytes the account holds now: repositories, container images and workspace data, added together. Measured on every read; there is no storage history. |
| **Repositories** | The unique bytes of the account's repositories. Content shared between repositories is counted once. |
| **Container images** | The unique bytes of the account's images (build cache included), each layer counted once across all tags. |
| **Workspace data** | The on-disk size of every dataset in each of the account's workspaces. |
| **Transfer** | Bytes uploaded to and downloaded from repositories (push, pull, chunk upload and read), counted per UTC hour. |
| **Builds** | Repository build compute plus image build time. |
| **Run time** | The time of every run in each workspace, training and experiment trials included. |
| **Repository storage limit** | The quota on repository storage. Above it, new repository writes are refused; what is stored stays readable. |
| **Estimated charge** | Transfer and repository build time at the prices the lake operator configured. |

## Open Usage

1. Open **Organization → Usage**. It opens on the account of your current workspace. For another
   account (an organization you belong to), enter its id in **Customer ID** and press **Show
   usage**, or open the organization's **Usage** tab and press **Open usage**.
2. Read the figures: **Stored**, **Transfer** (uploaded and downloaded over the hours shown),
   **Builds** and **Run time**.
3. The side column shows the **Repository storage limit** with a meter, and the **Estimated
   charge**.
4. Below: storage by kind (**Repositories**, **Container images**, **Workspace data**), compute, a
   **Workspaces** table (data, datasets, run time and runs per workspace, or why one could not be
   read), **Not Measured Yet**, and **Repository usage by hour**.

- REST: `GET /v1/metering_usage?customer_id=<customer id>`.
- MCP: `metering_usage` with the same input.

Narrow the hours with `from` and `to` (inclusive UTC hours) and `limit` (1 to 10,000, default 744):

- REST: `GET /v1/metering_usage?customer_id=<customer id>&from=2026-09-01T00:00:00Z&to=2026-09-30T23:00:00Z&limit=744`.

`from` and `to` also bound the image builds and workspace runs counted, by when each finished.
Storage is always the current figure.

## Set the Repository Storage Limit

You need `admin` on the account.

1. On **Organization → Usage**, press **Set storage limit** (or **Change** on the limit card).
2. Enter the **Limit in bytes (0 for no limit)**. The dialog shows it in readable units.
3. Press **Set limit**.

- REST: `POST /v1/set_storage_quota` with
  `{"customer_id": "<customer id>", "quota_bytes": 10737418240}`.
- MCP: `set_storage_quota`.

The value overrides the lake's default limit for that account; `0` removes the limit.

## What Happens over the Limit

- A push or upload that would take repository storage past the limit is refused as busy (HTTP 429),
  and repository metadata writes are throttled while storage is at or above it.
- Everything already stored stays complete and readable. Lake never replaces content with a pointer
  or a smaller copy.
- The Usage page shows a warning and the limit card turns red.

To write again, delete what you no longer need, or ask an admin of the account to raise the limit.

## The Estimated Charge

Prices are set by the lake operator, in whole micro-units of one currency, for inbound and outbound
transfer per GiB and for compute per second. The estimate applies them to the hours shown:

- only transfer and repository build time are priced; storage, images, image builds and workspace
  runs are measured but not priced;
- with no price set, the page says there is no charge to show.

How usage is billed is not decided; this page reports usage and an estimate only.

## What Is Not Measured Yet

The **Not Measured Yet** section lists, in plain sentences, what a report could not include. It can
name:

- repository storage over time (only the current size is measured);
- a workspace that is not running, or whose records could not be read;
- repositories that have no workspace;
- a workspace whose image tags could not be read;
- model files and indexes inside a workspace, whose size is not reported yet.

A workspace that could not be read is never counted as zero; it is named instead.

## Permissions

| Call | Needs |
|---|---|
| `metering_usage` | `read` on the account (any organization member, for the organization's account) |
| `set_storage_quota` | `admin` on the account |

Each read and each limit change is recorded in the central audit log.

## Limits

- Transfer is written from memory every few seconds; a usage read includes this server's pending
  counts, and another server's after its next write.
- Image build time runs from when the build was queued to when it finished, so queue time is
  included.
- A run without both a start and an end time counts nothing.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| **Choose a customer** | You are the operator, or no account is selected. Enter a **Customer ID**. |
| `unauthorized` | You have no grant on that account. |
| `invalid_input` on **Set limit** | The limit is not a whole number of bytes, or is negative. |
| Pushes answer 429 busy | Repository storage is at the limit. See [What Happens Over the Limit](#what-happens-over-the-limit). |
| A workspace shows a message instead of figures | It is not running or could not be read; its figures are left out, not counted as zero. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Workspaces](/docs/workspaces)
- [Repositories](/docs/repositories)
- [Packages, Builds and the Registry](/docs/registry)
- [Activity and Audit](/docs/activity)
