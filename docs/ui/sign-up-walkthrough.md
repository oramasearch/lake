# Fresh-profile sign-up walkthrough

Recorded on 2026-09-08 against native Lake with local PostgreSQL 17. Each run used a fresh
Chrome browser context and a separate client identity directory inside this worktree. The
walkthrough database (`lake_walkthrough_119`) was separate from the suite database
(`lake_test_119`). No operator bearer was supplied to any onboarding request. The existing
native Liquid binary supplied the documented identity export command.

## Follow the journey

1. Open `/sign-up` with no cookies. Follow **Install liquid** for prerequisites and the supported
   source build, or expand **Installation commands** on the page. The command sequence clones
   Liquid with submodules, configures with CMake and Ninja, builds with `-j 6`, installs to
   `$HOME/.local`, and adds its `bin` directory to `PATH`. Repository read access is required.
2. Run `liquid identity show --document`. On a fresh client profile, Liquid creates the signing
   identity locally and prints only the public document. In this proof each profile set
   `LIQUID_HOME` to its own directory. No key file was read or sent to Lake.
3. Enter email, display name, a password of at least 12 characters, and device name. Paste only
   the first 40 characters of the public document and submit to exercise recovery. The page
   returns the message below and creates no account.
4. Run the same command again and paste its complete output, without quotes. Fill the remaining
   form fields and select **Create account**. The response sets an HttpOnly session cookie and
   redirects to `/repositories`; its customer selector contains the new personal handle.
5. Before writing any repository content, call `GET /v1/metering_usage?customer_id=<id>` with that
   browser session. The customer ID is the `id` in `GET /v1/list_customers`. Confirm the hourly
   `stored_bytes`, `transfer_in_bytes`, `transfer_out_bytes`, `compute_seconds`, and `cost_micros`
   are all `0`. This run configured all four unit prices to `1000` and the storage quota to
   `1073741824` bytes, so zero cost cannot be explained by free prices. No customer quota
   override was present.
6. In **Create a repository**, keep the personal customer, enter `my-first-repository`, and
   optionally describe it. Select **Create repository**. The form calls the registry capability
   with private visibility and redirects to `/repository?id=<id>`. The repository home displays
   its generated customer handle, the repository name, private visibility, and inherited `admin`
   access for the client identity.
7. Repeat in fresh profiles at 1280 and 640 pixels, in light and dark. All four runs completed;
   each final page's document width matched the viewport width. The shared narrow navigation
   retains its existing horizontal scrolling behavior.

## Recovery message

> TAAP identity document is incomplete or invalid canonical CBOR: declared length exceeds input. Run liquid identity show --document and paste the complete output.

Invalid alphabet input also returns a specific base64url message with the same export command.
Signatures and canonical encoding remain verified by libliquid before account creation.

## Recorded customers and zero usage

The separate walkthrough database contains exactly four customers, four users, and four private
repositories: one for each profile. Each customer's zero usage was recorded before its first
repository was created.

| Width | Theme | Personal customer | Initial stored bytes | Initial cost (micro-units) |
|---|---|---|---:|---:|
| 1280 | light | `silver-fjord-234` | 0 | 0 |
| 1280 | dark | `coral-anchor-913` | 0 | 0 |
| 640 | light | `zesty-marble-162` | 0 | 0 |
| 640 | dark | `coral-summit-899` | 0 | 0 |

```sql
SELECT count(*) FROM customers;
-- 4
SELECT c.name, m.stored_bytes, m.transfer_in_bytes, m.transfer_out_bytes,
       m.compute_seconds
FROM customers c JOIN metering_usage m ON m.customer_id = c.id
ORDER BY c.name;
-- Four rows; every usage field is 0.
```

## Screenshots

| Profile | Sign-up instructions | Malformed document | First-repository form | Repository home |
|---|---|---|---|---|
| 1280-light | [Instructions](screenshots/sign-up/1280-light-sign-up.png) | [Recovery](screenshots/sign-up/1280-light-malformed.png) | [Form](screenshots/sign-up/1280-light-repositories.png) | [Repository](screenshots/sign-up/1280-light-first-repository.png) |
| 1280-dark | [Instructions](screenshots/sign-up/1280-dark-sign-up.png) | [Recovery](screenshots/sign-up/1280-dark-malformed.png) | [Form](screenshots/sign-up/1280-dark-repositories.png) | [Repository](screenshots/sign-up/1280-dark-first-repository.png) |
| 640-light | [Instructions](screenshots/sign-up/640-light-sign-up.png) | [Recovery](screenshots/sign-up/640-light-malformed.png) | [Form](screenshots/sign-up/640-light-repositories.png) | [Repository](screenshots/sign-up/640-light-first-repository.png) |
| 640-dark | [Instructions](screenshots/sign-up/640-dark-sign-up.png) | [Recovery](screenshots/sign-up/640-dark-malformed.png) | [Form](screenshots/sign-up/640-dark-repositories.png) | [Repository](screenshots/sign-up/640-dark-first-repository.png) |

## Native proof

Run the full suite with `LAKE_TEST_DB_URI` pointing at a fresh schema-loaded database and
`LAKE_INSTANCE_KEK` set. Use a separate database for a manual server; test fixtures own their
instance identity and must not share that database with a server using another key.

```sh
cmake -S . -B build -G Ninja \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build -j 6
ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build-shared -j 6
ctest --test-dir build-shared --output-on-failure
```

Final suite summaries are recorded after the branch is rebased and reverified.
Linux proof deferred: the designated `tools/linux-proof.sh` helper is absent on this host.
No image build was attempted; no Docker on this host.

Browser validation also accepted `my-first-repository` and `a`, and refused `Bad Name`,
`-bad`, and `bad-` with the same handle rules enforced by the registry.

The homepage and navigation work remain #135; MCP schema conformance remains #124.
