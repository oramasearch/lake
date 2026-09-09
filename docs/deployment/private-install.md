# Private instance clone and push

Configure the external origin customers see in their browser. It includes the scheme and any
non-default port. If `LAKE_EXTERNAL_ORIGIN` is unset, Lake uses `https://` plus
`LAKE_PUBLIC_HOST`, the same hostname used for OAuth callbacks. Set the explicit origin for HTTP or
a non-default port. Clone and push use the existing capability-backed repository sync protocol.

## Native local instance

Build Lake using [the native build instructions](../build/local.md). Use a fresh PostgreSQL 17
database, private store directory, and credentials belonging only to this instance:

```sh
createdb -h 127.0.0.1 lake_private_demo
psql -h 127.0.0.1 -d lake_private_demo -v ON_ERROR_STOP=1 -f db/schema.sql
export LAKE_CONTROL_DB_URI="postgresql://stevesperandeo@127.0.0.1:5432/lake_private_demo"
export LAKE_INSTANCE_KEK="$(openssl rand -hex 32)"
export LAKE_ADMIN_TOKEN="$(openssl rand -hex 32)"
export LAKE_STORE_ROOT="$PWD/.cache/private-demo/store"
export LAKE_EXTERNAL_ORIGIN=http://127.0.0.1:49127
export LAKE_LOG_MODE=air-gapped
./build/lake --listen 127.0.0.1:49127
```

Use a database name that does not already exist. Keep the generated keys for as long as you keep
the database. This local example uses the Homebrew role `stevesperandeo`; select the database role
provisioned for your installation. With a TLS proxy, set `LAKE_EXTERNAL_ORIGIN` to the external
HTTPS origin, for example `https://code.example.internal:8443`, while keeping the internal bind
address separate. See [configuration](../serving/configuration.md) for accepted values.

Open the configured origin, sign up, create a private repository, and open **Tokens** to issue
an account token on this same instance. In a second terminal, make that token available to Liquid
without putting it in a URL:

```sh
read -r -s LIQUID_TOKEN
export LIQUID_TOKEN
```

Paste the token when `read` waits for input. Copy the repository page's first **Clone and push**
block. It runs `liquid clone '<instance>/v1/repositories/<id>' './<name>'` and `cd './<name>'`.
An empty remote creates an empty checkout; a populated remote materializes its files. The initial
block requires a new or empty destination directory, not an existing Liquid repository.

After capturing and naming changes in that checkout, copy the panel's `liquid push` block. For an
explicit edit, `liquid refs --json` supplies the published `main` reference's `id` for
`liquid write <path> --from <prepared-file> --base <id>`; then run `liquid cut '<message>'`.
Clone again into a separate directory and use `cmp` to compare the edited file with its source.
All browser assets, API calls, clone and push requests stay on the configured private origin.
Air-gapped log mode requires no witness or aggregation connection. No hosted-service connection
is required for this workflow.

## Executable browser-to-terminal proof

Build the pinned Liquid CLI separately so Lake continues to consume its library-only submodule:

```sh
cmake -S modules/liquid -B build-liquid -G Ninja -DLIQUID_BUILD_TESTS=OFF
cmake --build build-liquid --target liquid-cli -j 6
```

The proof requires Node, Puppeteer, native Chromium, `psql`, the built Lake and Liquid binaries,
and a PostgreSQL 17 connection selected by `LAKE_TEST_DB_URI` with a role that can create databases.
Puppeteer may be installed in a local tool directory; point `LAKE_TEST_PUPPETEER` at its module entry file if it is
not resolvable as `puppeteer`. Set `LAKE_TEST_BROWSER` to a native Chromium executable when the
Puppeteer default browser is unavailable.

```sh
export LAKE_TEST_LIQUID="$PWD/build-liquid/liquid"
export LAKE_ORIGIN_EVIDENCE="$PWD/.cache/instance-origin-proof"
ctest --test-dir build --output-on-failure -R '^InstanceOrigin.BrowserToTerminal$'
ctest --test-dir build --output-on-failure
ctest --test-dir build-shared --output-on-failure
```

The C++ test obtains its fresh database from `lake::test::postgres_uri()` and its filesystem root
from `lake::test::fixture_path()`, then passes both to the browser/terminal driver. The shared test
runner loads the schema and drops only that private database; the caller's database is untouched.
The driver uses its own store, signing identity, browser profile and bearer. It overrides a
deliberately different environment origin with the explicit flag. The test opens the page on a non-default
loopback port and saves `repository.html`, `panel.txt`, `browser-requests.txt` and `terminal.txt` in the
evidence directory. Browser requests to any other origin are refused. Before running any copied
shell text, it checks every command and URL against the created instance and repository.

The first comparison checks a 4,096-byte binary file and an empty file. After a binary edit and
the copied push command, a second fresh clone must contain the new bytes. The test fails on any
unexpected origin or byte difference. CTest skips this external-client scenario only when
`LAKE_TEST_DB_URI` or `LAKE_TEST_LIQUID` is absent; set both for the complete PostgreSQL proof.
Run native static and shared suites one at a time. No container runtime is involved.

Change the rendered commands in `src/api/web_ui.cpp`, their trusted input in `ServerOptions`,
and this test together. Renderer and startup tests cover custom HTTPS origins, explicit ports,
missing configuration, invalid origins and shell/HTML escaping. The browser proof is required
because string-only checks cannot establish that the displayed commands work in a terminal.
