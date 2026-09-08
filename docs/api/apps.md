# Application authentication API

## What

Lake applications use the identity core as a service. Registering an application creates an
isolated `pool_id`, an `application` policy object beneath its customer, and an Ed25519 signing
key. Password, TOTP, recovery-code, passkey, OAuth, and organization SSO capabilities already
take a pool and use the same credential, device, session, and identity-document tables as lake's
own `lake` pool. A credential from one pool is never considered by another pool.

The registration records a display name, exact allowed origins, redirect URIs, an optional custom
sign-in host, and a JSON theme. Browser requests carrying an `Origin` header must come from the
hosted page itself or an allowed origin. Redirects use only a registered URI. The custom host is a
DNS and ingress concern: route that host to lake with a valid certificate, then lake selects the
application by `Host`.

## Capabilities

All protected capabilities resolve the customer or `application` policy object and pass through
`authorize`. `Registry::call` appends the signed audit event with `audit::record` for success,
denial, and failure.

| Capability | Authorization | Result |
|---|---|---|
| `register_application` | customer `admin` | application, including its new `pool_id` |
| `list_applications`, `get_application` | customer/application `read` | registrations without key material |
| `update_application_theme` | application `admin` | updated registration |
| `rotate_application_signing_key` | application `admin` | new key ID and version |
| `application_jwks` | public | all retained public verification keys |
| `issue_application_jwt` | application `write` | short-lived user access token |
| `begin_application_authorization` | application `write` | one-use code, state, and exact registered redirect for the signed-in lake user |
| `exchange_application_authorization_code` | public client | EdDSA access and ID tokens after exact redirect and PKCE S256 verification |
| `list_application_users`, `export_application_user` | application `read` | pool user profiles and status |
| `suspend_application_user`, `delete_application_user` | application `admin` | suspended profile or deletion confirmation |
| `list_application_organizations` | application `read` | application-owned organizations |
| `create_application_organization`, `update_application_organization`, `delete_application_organization` | application `admin` | organization lifecycle result |
| `create_application_webhook` | application `admin` | webhook registration and its secret, returned once |
| `list_application_webhooks`, `list_application_webhook_events` | application `read` | secret-free registrations, events, and delivery attempts |
| `retry_application_webhook` | application `admin` | the last appended attempt |

The existing public identity capabilities accept the application's `pool_id`. A successful
application sign-up or sign-in adds `access_token`, `token_type: Bearer`, and `expires_in` to the
ordinary session result. API-token issuance through `create_token` returns only the derived
session bearer, bounded by issuer scopes, expiry and ancestry; it does not also mint an independent
application JWT. Its `session.started` lifecycle event is retained. The derivation policy and REST/MCP
PostgreSQL coverage are in [identity.md](identity.md#token-derivation-and-revocation). Suspending a user revokes that user's sessions in this pool immediately;
deleting one removes this pool's credentials and sessions. Neither action touches another pool.

## Hosted pages and HTTP routes

| Route | Purpose |
|---|---|
| `GET /applications`, `POST /applications` | list and register applications |
| `GET /application?id=<id>` | registration and links to management pages |
| `GET /application/users?id=<id>`, `POST /application/users/suspend` | list and suspend pool users |
| `GET /application/organizations?id=<id>` and its POST actions | list, create, and delete organizations |
| `GET /application/events?id=<id>`, `POST /application/webhooks` | event log and webhook registration |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed hosted identity pages on lake's host |
| the same `/sign-up`, `/sign-in`, and `/mfa` paths on the custom host | themed custom-domain entry pages |
| `GET /v1/apps/<id>/.well-known/jwks.json` | raw public JWKS, cached for five minutes |

The form adapters call the registered identity capabilities with the application pool. Sign-up,
password sign-in, and TOTP verification set the same HttpOnly session cookie as lake identity.
When a redirect URI is configured, successful password, MFA, passkey, and OAuth authentication
returns `access_token`, `token_type`, and `expires_in` in its URI fragment; fragments are not sent
to the redirect server and the application reads and clears them in its own page. Lake never
redirects to a URI outside the registration.
The hosted passkey page contains the sole application-page script: the documented
`navigator.credentials.get` WebAuthn call and its JSON posts. The other application identity
pages contain no script. Theme colors accept hexadecimal CSS colors; all application values are
HTML-escaped.

## JWT contract and verification

Tokens use `alg: EdDSA` and a `kid` present in the application's JWKS. Claims include `iss`, the
user TAAP identity as `sub` (or the user UUID if no TAAP identity exists), the application UUID as
`aud`, `application_id`, `pool_id`, `user_id`, `iat`, `exp`, and `jti`. The default lifetime is 15
minutes and cannot exceed one hour. A rotation makes a new key current but retains retired public
keys so already-issued tokens continue to verify.

The OIDC code path uses `begin_application_authorization` after lake session resolution. It
registers the lake user in the application, requires an exact configured redirect, accepts only
PKCE S256, and persists only the code hash with a five-minute expiry. Exchange consumes the code
before checking its redirect and verifier, so every completion attempt is one-use. The ID token
adds the request nonce and uses `https://<application host>/v1/apps/<id>` as issuer, the application
UUID as audience, and the same keys published by `application_jwks` and the raw JWKS route.

Always require EdDSA, fetch keys from the exact application JWKS URL, and compare `aud` to the
application UUID. Common runtime libraries perform the key selection from `kid`:

```js
// Node.js with jose
const jwks = createRemoteJWKSet(new URL(jwksUrl))
const { payload } = await jwtVerify(token, jwks, {
  algorithms: ['EdDSA'],
  audience: applicationId,
  issuer
})
```

```python
# Python with PyJWT
key = jwt.PyJWKClient(jwks_url).get_signing_key_from_jwt(token)
claims = jwt.decode(token, key.key, algorithms=["EdDSA"],
                    audience=application_id, issuer=issuer)
```

```go
// Go with github.com/golang-jwt/jwt/v5; keyFunc selects kid from the fetched JWKS.
parsed, err := jwt.Parse(token, keyFunc,
    jwt.WithValidMethods([]string{"EdDSA"}),
    jwt.WithAudience(applicationID), jwt.WithIssuer(issuer))
```

Rust applications can use `jsonwebtoken` with an EdDSA decoding key selected from the JWKS and
set `Validation.algorithms`, `Validation.aud`, and `Validation.iss` before decoding.

## Lifecycle webhooks

Supported event names are `user.created`, `user.updated`, `user.deleted`,
`organization.created`, `organization.updated`, `organization.deleted`, `session.started`, and
`mfa.changed`. The JSON body is:

```json
{"id":"event UUID","application_id":"application UUID","event":"session.started","created_at":"timestamp","data":{}}
```

Lake sends `Content-Type`, `X-Lake-Event`, `X-Lake-Event-Id`, and `X-Lake-Signature`. The signature
is lowercase hex `v1=HMAC-SHA256(secret, event_id + "." + exact_body)`. Compare it in constant
time before parsing or acting on the event. A non-2xx response or transport failure is retained
and retried up to three consecutive attempts. `retry_application_webhook` appends another bounded
set without changing earlier attempts. The event log retains status, signature, error, and attempt
number for each delivery.

Signing seeds and webhook secrets are AES-256-GCM protected through application-specific keystore
purposes before Postgres storage. They never appear in list, application, event, or audit output.

## How to verify

```sh
cmake --build build
LAKE_TEST_DB_URI=postgresql://lake:password@127.0.0.1:5432/lake_test \
  ctest --test-dir build --output-on-failure -R 'ApplicationAuth|ApplicationHttp|PostgresApplications'
```

`ApplicationAuth` verifies a real Ed25519 JWT against the returned JWKS and checks its audience,
records a failed webhook delivery followed by its successful retry, suspends the user, and proves
pool isolation. Its policy case proves application lists filter inaccessible registrations and
reject access to another application's user list. `ApplicationHttp` submits the hosted forms,
renders every custom-domain entry page, and reads the raw JWKS route.
`PostgresApplications` exercises the registry capabilities against migration `0016_auth_apps`.
`IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` starts from a liquid TAAP-key lake
session, completes the PKCE code exchange as a test relying party, and validates the ID token
signature and claims against the application's JWKS.

## How to change

Keep pool authentication in the identity core. Add application records through the `Store`
interface and both store implementations, expose behavior as a registry capability with an
application policy-object resolver, and let pages call that capability. Add a lifecycle event only
with its exact payload, webhook test, audit outcome, and this contract updated together.
