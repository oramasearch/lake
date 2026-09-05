# Customer metering

**Status: implemented.** `metering_usage` reports hourly storage, transfer, and build-compute
usage for one authorized customer. The same report is available at
`GET /metering?customer_id=<uuid>`.

## Usage model

`metering_usage` is keyed by customer and UTC hour. `stored_bytes` is a gauge sampled from the
customer's libliquid store; transfer and compute are counters.

| Field | Rule |
|---|---|
| `stored_bytes` | Sum the uncompressed lengths in libliquid pack-frame metadata once per digest across `LAKE_STORE_ROOT/<customer>/store`. Content-addressed duplicates are counted once. |
| `transfer_in_bytes` | Count binary request-body bytes accepted by missing-object, push, pull, and chunk-upload paths. |
| `transfer_out_bytes` | Count binary response bytes returned by missing-object, push, pull, and ranged chunk-read paths. |
| `compute_seconds` | Sum sandbox, execution, hash, and store milliseconds from each build result, round each result up to a whole second, and record each result ID once. Queue time is excluded. |

Reading usage reconciles build records from the customer's repositories into the idempotency
ledger before returning the hourly rows. This lets a repeated report safely observe the same build
history. Migration `0014_metering` owns `metering_usage`, `metering_build_records`, and
`metering_guard`.

## Capability

`GET /v1/metering_usage` accepts:

```json
{"customer_id":"<uuid>","from":"2026-09-01T00:00:00Z","to":"2026-09-30T23:00:00Z","limit":744}
```

`from` and `to` are optional inclusive UTC-hour bounds. `limit` defaults to 744 and is bounded to
1–10,000. The result contains `items`, configured pricing, the total integer `cost_micros`, the
storage quota, and an over-quota warning. Authorization resolves the customer policy object and
calls `authorize` for `read`. Registry dispatch records the read through `audit::record` as
`metering.usage.read` with the observed stored-byte count and quota state.

## Pricing configuration

Prices are integer micro-units in `LAKE_PRICE_CURRENCY` (default `USD`):

| Environment variable | Unit |
|---|---|
| `LAKE_PRICE_STORAGE_GIB_HOUR_MICROS` | one GiB-hour |
| `LAKE_PRICE_TRANSFER_IN_GIB_MICROS` | one inbound GiB |
| `LAKE_PRICE_TRANSFER_OUT_GIB_MICROS` | one outbound GiB |
| `LAKE_PRICE_COMPUTE_SECOND_MICROS` | one compute second |
| `LAKE_STORAGE_QUOTA_BYTES` | unique stored-byte quota; `0` disables the quota |

Fractional byte-based charges round up to one micro-unit. Arithmetic saturates at the unsigned
64-bit maximum rather than wrapping.

## Quota behavior

Chunk upload computes the exact raw bytes for digests absent from the customer store while holding
the customer-store write lock. A write that would cross the configured quota returns liquid
`busy`, rendered as HTTP 429. Metadata writes are also throttled while the store is at or above the
quota. Existing objects remain readable and complete. Lake never replaces content with pointer
files or another reduced representation. Quota refusal and its rule are included in the signed
capability audit event.

## How to verify

`Metering.*` proves exact storage reporting, hourly pricing, and idempotent build accounting.
`PostgresMetering.*` exercises the production aggregate and build ledger. `SyncHttp.StorageQuota*`
pushes existing content, enables a quota, proves a new write returns `busy`, and then pulls the
original bytes unchanged. `WebUi.Metering*` covers the renderer.
