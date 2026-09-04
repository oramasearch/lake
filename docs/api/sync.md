# Repository sync

## What

lake exposes the repository synchronization capabilities through the one capability registry:
`refs` advertises a repository’s ref heads, `find_missing` returns requested chunk digests absent
from that repository customer’s libliquid store, and `push` / `pull` carry signed drips, changes,
and pack-framed chunks.

Each addressed repository resolves to the liquid store at
`$LAKE_STORE_ROOT/<customer id>/store`. `find_missing` is a metadata action: it accepts
`repository_id` and `digests`, then returns `{digests: [...]}` without transferring contents.
`refs` is a read taking `repository_id` and returning `{items: [{name, head, updated_at}]}`.

## Protocol status

The authoritative body framing, cursor rules, signature record, and pack framing are being added
by liquid #160 at `docs/library/sync.md`. This page must mirror that document byte for byte at the
protocol level; it intentionally does not select a provisional encoding. The current lake pin is
liquid `main` before that public sync API lands, so lake provides the customer-store lookup and
missing-chunk capability now. Once #160 merges, lake advances the submodule pin, implements the
streaming `push` and `pull` adapter handlers against the published `sync_session`, and runs the
liquid CLI round trip.

## Authorization and integrity

Every sync capability passes through `Registry::call`. The repository action invokes
`authorize(principal, repository, action)` before reading or writing data. Until lake #16/#19 is
rebased, the temporary authorizer permits only the existing `admin` principal and denies any other
principal. liquid #160 supplies the verified import operation: bad TAAP signatures reject the
entire drip batch before refs are published.

## Verify

Run `ctest --test-dir build --output-on-failure`. The registry test proves that missing-chunk
negotiation selects the repository customer’s store and returns only absent digests. After liquid
#160 is pinned, add the client round trip, bad-signature rejection, and second-push byte-count
checks described in that issue.

## Change

Do not change a field, cursor rule, digest encoding, or pack record only in lake. Update liquid
`docs/library/sync.md` first, mirror it here, add shared conformance vectors, then update both
implementations in the same review cycle.
