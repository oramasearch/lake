# Customer liquid-store volume

## What

lake opens one libliquid store per customer at
`$LAKE_STORE_ROOT/<customer id>/store`. The root is an operator-provided read-write PersistentVolume
mounted into every Lake pod; lake does not choose a storage class or create a claim itself.

## Why

The liquid store owns its pack files, index runs, manifests, and collector state. Keeping a store
per customer makes customer ownership explicit and lets the control plane address a repository’s
data through its `customer_id`. It also preserves the library boundary: lake links `liquid::liquid`
with `LIQUID_BUILD_CLI=OFF` and never changes liquid through its lake submodule.

## Operator contract

`orama-operator#132` owns the `Lake` CR volume field, PersistentVolumeClaim reference, mount, and
`LAKE_STORE_ROOT` injection. The required mount must be writable by lake’s numeric runtime user
`10001:10001`. A multiple-replica deployment needs a volume access mode compatible with the
operator’s scheduling and the libliquid store’s process locking; do not enable shared concurrent
writers without the operator’s documented placement guarantee.

The operator also vendors the byte-identical `db/schema.sql` asset. Migration
`0006_repository_lifecycle` changes its `PostgresSchema` guard table to
`repository_lifecycle_events`.

## Verify

Start a local lake with an explicit writable directory:

```sh
LAKE_STORE_ROOT="$PWD/test-store" \
  ./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t
```

After a sync request for customer `<id>`, confirm that the store path is
`test-store/<id>/store`. Run `ctest --test-dir build --output-on-failure` for the capability and
renderer checks. The operator PR adds the mounted-volume integration test.

## Change

Do not change the root layout, user ID, or volume access assumptions only in the deployment
manifest. Update the `Lake` CR reference, this page, `README.md`, `CLAUDE.md`, and the operator
runbook together. Store-format changes belong to liquid and reach lake through a pinned submodule
commit.
