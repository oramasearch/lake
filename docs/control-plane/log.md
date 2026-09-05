# Transparency-log operator handoff

Lake owns runtime validation and checkpoint behavior. The Orama operator owns the `Lake` custom
resource and must map its desired-state fields to the environment below. This page is the
cross-repository contract for that mapping; it does not add a Lake CRD to this repository.

## Required mapping

| Lake CR field | Pod environment | Source |
|---|---|---|
| `spec.transparencyLog.mode` | `LAKE_LOG_MODE` | Literal `air-gapped`, `witnessed`, or `anchored`; omit only when the operator deliberately accepts Lake's witnessed default. |
| `spec.transparencyLog.witnesses` | `LAKE_LOG_WITNESSES` | JSON-encoded array of endpoint and public-key records. Public keys are public configuration, not Kubernetes Secrets. |
| `spec.transparencyLog.witnessThreshold` | `LAKE_LOG_WITNESS_THRESHOLD` | Decimal integer. |
| `spec.transparencyLog.aggregator.endpoint` | `LAKE_LOG_AGGREGATOR` | Literal endpoint. Required for anchored mode. |

The operator must place `LAKE_STORE_ROOT` on a writable persistent volume before enabling any
networked log mode. Lake accepts HTTPS endpoints. It accepts loopback HTTP only for local tests.
The operator must not inject witness private keys, an aggregator credential, or a value that lets
the pod mint a witness cosignature; Lake verifies externally supplied cosignatures with configured
public keys.

## Example

```yaml
apiVersion: orama.cloud/v1alpha1
kind: Lake
metadata:
  name: regulated
spec:
  transparencyLog:
    mode: anchored
    witnessThreshold: 2
    witnesses:
      - endpoint: https://witness-a.example/checkpoint
        publicKey: 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
      - endpoint: https://witness-b.example/checkpoint
        publicKey: 2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40
    aggregator:
      endpoint: https://calendar.example/aggregate
```

The resulting container environment is:

```text
LAKE_LOG_MODE=anchored
LAKE_LOG_WITNESSES=[{"endpoint":"https://witness-a.example/checkpoint","public_key":"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"},{"endpoint":"https://witness-b.example/checkpoint","public_key":"2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40"}]
LAKE_LOG_WITNESS_THRESHOLD=2
LAKE_LOG_AGGREGATOR=https://calendar.example/aggregate
```

Lake refuses to start if anchored mode lacks an aggregation endpoint or if the configured witness
set cannot meet its threshold. Air-gapped mode has no witness or aggregator values and makes no
outbound calls. A checkpoint is current only after the configured witness threshold has signed it;
anchored mode additionally stores the aggregator's proof.

## Verify

After the operator creates the pod, inspect its effective environment and call:

```sh
curl -H "Authorization: Bearer $LAKE_ADMIN_TOKEN" \
  https://<lake>/v1/transparency_log_configuration
```

The result must show the selected mode, public witness endpoints and keys, threshold, and the
aggregation endpoint in anchored mode. Publish a checkpoint through
`POST /v1/publish_transparency_checkpoint`; confirm the record appears at
`LAKE_STORE_ROOT/<instance>/log/checkpoints/current.json` and, for anchored mode, has a matching
`.ots.json` proof object.

## Change

The operator implementation belongs to its own pull request. If it changes a field name or the
environment encoding, update this page and `docs/api/transparency.md` in the same Lake change.
