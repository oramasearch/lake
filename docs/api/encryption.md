# Encryption as a service

**What.** Each secret mount can hold named, versioned encryption keys. Clients send data to a
capability and receive ciphertext, a signature, a verification result or an HMAC; key material is
never returned. Rotation changes the current version while keeping prior versions available for
decrypt and verify.

## Capabilities

All mount operations require `read` or `write` on the mount policy object. `list_encryption_keys`
authorizes its requested mount through `authorize_batch` before it returns any metadata. Customer
KEK rotation requires `admin` on the customer.

| Capability | Input | Result |
|---|---|---|
| `create_encryption_key` | `mount_id`, lowercase `name` | key id and version 1 metadata |
| `list_encryption_keys`, `get_encryption_key` | mount and optional name/limit | metadata only |
| `rotate_encryption_key` | `mount_id`, `name` | metadata with the new current version |
| `encrypt` | mount, name, base64url `plaintext` | `lake:v1:<key-id>:<version>:<sealed>` |
| `decrypt` | mount, name, lake `ciphertext` | base64url plaintext and embedded key version |
| `rewrap` | mount, name, lake `ciphertext` | ciphertext under the current version |
| `sign` | mount, name, base64url `message`, `version?` | base64url Ed25519 signature |
| `verify` | mount, name, message, signature, `version?` | `{valid, key_version, algorithm}` |
| `hmac` | mount, name, base64url `message`, `version?` | base64url HMAC-SHA-256 |
| `derive_encryption_key` | mount, `parent_name`, child `name`, base64url `context` | stored child-key metadata and parent version |
| `rotate_customer_kek` | `customer_id` | new KEK version and count of rewrapped data keys |

Omitting `version` selects the current named-key version. Encrypt always uses the current version.
Decrypt and rewrap reject an envelope whose key id does not match the named key. Rewrap decrypts
with the embedded old version and encrypts with the current version, so plaintext never crosses
the API boundary during rotation. Derivation uses HMAC-SHA-256 over the parent material with a
domain label and the supplied context, then stores the child as an ordinary versioned key.

## Envelope hierarchy

```text
LAKE_INSTANCE_KEK
  └─ customer KEK in keystore
       ├─ mount data key → static secret versions
       └─ named data key version → encrypt / sign / HMAC / derive
```

The Postgres service requires `LAKE_INSTANCE_KEK` to be exactly 32 bytes encoded as 64 hexadecimal
characters. The instance key wraps each customer KEK in `keystore`; the customer KEK wraps mount
and named-key material in `secrets_data_keys`. Protected payloads use AES-256-GCM with random
96-bit nonces and associated data. Transit encryption derives an AES key from the named material
with a fixed domain label. Signatures are Ed25519; MACs and child derivation use HMAC-SHA-256.

Named-key rotation inserts a new wrapped data-key version and advances `current_version`. Old data
keys remain wrapped, allowing old ciphertext to decrypt. Customer-KEK rotation generates a new
KEK, unwraps and rewraps every `secrets_data_keys` row in one transaction, and replaces only the
wrapped keys plus the KEK row. It does not update `secrets_versions`, so static ciphertext is not
re-encrypted. The store interface keeps this boundary suitable for an HSM-backed KEK provider.

Each operation is one signed audit event through `audit::record`, with caller, customer, mount,
key and selected version. Plaintext, ciphertext, messages, signatures and MACs are excluded from
audit details.

## Page and verification

`GET /encryption-keys?mount_id=<uuid>` shows named-key metadata and rotation state without key
material. The rotation test proves new ciphertext uses the new version, old ciphertext still
decrypts, rewrap advances it, and customer-KEK rotation leaves both secrets and transit
ciphertexts usable. The Postgres integration repeats the complete capability round trip.
