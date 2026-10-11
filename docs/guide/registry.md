# Packages, Builds and the Registry

Lake has its own package registry, so the images your apps run and the packages your code installs
come from the same place as your repositories. **Ship → Packages** shows what it holds: container
images, npm packages and Python distributions, each with its versions and the command that
installs it, and the image builds of the current workspace.

Builds turn a lake repository into a container image without any machine of your own: lake records
the build, the Orama operator runs it inside the cluster, and the image lands in lake's registry,
ready for an app.

Calls on this page name `customer_id`, the id of the account that owns your workspaces (yours, or
your organization's), and usually `workspace_id`, the workspace the build or image belongs to.

## Concepts

| Term | What it is |
|---|---|
| **Container image** | An OCI image in lake's registry. Your images are named `<handle>/<name>`, where `<handle>` is your account's handle, and belong to one workspace. |
| **Image repository** | One image name, `<handle>/<name>`, with its tags. Lake routes each to one workspace, which holds its manifests and tags; the name never moves. |
| **npm package**, **Python distribution** | Packages published to the lake instance. They are public reads: anyone can install them. |
| **Build** | One in-cluster image build: a repository, a revision, a Dockerfile and a target `name:tag`. Its phases are queued, fetching, building, pushing and done, or it fails or is cancelled. |
| **App build** | A build of a Ship app's own image from the app runtime, with the app's dependencies installed ([Apps](/docs/apps#build-the-apps-image)). |
| **Pull key** | A lake API token with the scope `registry_pull`, which can pull your private images and nothing else. |

## Ship → Packages

The page opens with figures (**Container images**, **npm packages**, **Python distributions**,
**Image builds**) and four tabs:

- **Container Images**, **npm** and **Python**: a card per package with its versions as chips and an
  **Install** command to copy. A search box filters by name.
- **Builds**: the image builds of the workspace in the page address, each with its target, phase
  and revision, its **Live log**, and **Browse messages** to read its progress topic in Train →
  Streams.

The Container Images tab shows the instance's public images and only the private images of your
account that you may read. Internal build-cache names are hidden.

## Build an Image from a Repository

1. Open the repository (**Data → Repositories**, then the repository).
2. Press **Build image**. Lake queues a build of the repository's default reference with the target
   `<repository name>:latest`, from the repository's own workspace.
3. The workspace's **Ship → Packages** page opens; the build is on the **Builds** tab, with its log
   as it runs.

- REST: `POST /v1/build_image`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "repository": "<repository id>", "revision": "main", "target": "reports:latest"}
```

- MCP: `build_image` with the same input.

`build_image` also takes `context` (the build directory, default `.`), `dockerfile` (default
`Dockerfile`) and `build_args` (an object of build arguments):

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "repository": "<repository id>", "revision": "<change id>", "target": "reports:v2", "context": "services/reports", "dockerfile": "Dockerfile.prod", "build_args": {"PYTHON_VERSION": "3.12"}}
```

The answer is the new `build_id`; the call returns at once and the build runs on its own.

### Where the Image Goes

You give only `name:tag`. Inside the cluster lake pushes it under your account's handle. Once the
build succeeds, `get_build` and `list_builds` answer its `image` as an immutable reference through
the lake's public host:

```
<lake host>/<handle>/<name>@sha256:<digest>
```

That is the reference to deploy: an app's **Deploy build** button and ApplicationAgent use it.
Until the build succeeds, `image` is null.

## Follow and Cancel a Build

On **Ship → Packages → Builds** (or an app's **Builds** tab), open **Live log** to follow a build.
An app's build also offers **Cancel build** and, once it produced an image, **Deploy build**.

- REST: `GET /v1/list_builds?customer_id=<customer id>&workspace_id=<workspace id>`
- REST: `GET /v1/get_build?customer_id=<customer id>&workspace_id=<workspace id>&build_id=<build id>`
- REST: `GET /v1/build_logs?customer_id=<customer id>&workspace_id=<workspace id>&build_id=<build id>` answers the retained log lines, also after the build's pod is gone.
- REST: `GET /v1/receive_build_events?customer_id=<customer id>&workspace_id=<workspace id>&build_id=<build id>&from_sequence=1&wait_ms=25000` waits for new progress from a sequence; pass the answer's `next_sequence` next time.
- To cancel, REST `POST /v1/cancel_build`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "build_id": "<build id>"}
```

- MCP: `list_builds`, `get_build`, `build_logs`, `receive_build_events`, `cancel_build`.

A build's progress is the topic `build-events.<build id>` in the workspace, kept for 24 hours and up
to 5,000 events. Cancelling sets the build's desired state to cancelled; the operator stops it.

## Build an App's Image

An app's own image is built with `build_app` rather than `build_image`: it starts from lake's app
runtime image, installs the app's dependencies, and runs as an unprivileged user. Use it from the
app page's **Builds** tab, or REST `POST /v1/build_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app": "<app id>"}
```

Once an app runs a built image, the next `build_app` needs `source` (`repository`, `revision`,
optional `path`), since that deploy no longer carries the repository. On a lake whose operator has
not configured the app runtime image, `build_app` answers `store_unavailable` naming
`LAKE_APPS_RUNTIME_IMAGE`.

## Pull Your Images

Your images are private to their account and workspace; anonymous pulls are refused.

1. Make a pull key: open **Tokens** from your account menu, press **New token**, and enter
   `registry_pull` under **Capability scopes, comma separated**. Or REST `POST /v1/create_token`:

```json
{"name": "image pulls", "scopes": ["registry_pull"]}
```

2. Log in with the user name `lake` and the key as the password, then pull by the reference
   **Install** shows or a build's `image`:

```
docker login <lake host> -u lake
docker pull <lake host>/<handle>/<name>:<tag>
```

A pull key reads only images of accounts its owner may read. An app pulls its own image with a
credential the operator holds for that app alone; you never handle it.

## Install npm and Python Packages

The **Install** command on each card points at this lake:

```
npm install <package> --registry https://<lake host>
pip install <project> --index-url https://<lake host>/simple/
```

These are public reads. Publishing npm and Python packages, and images under a one-part name, needs
the lake operator's administrator credential.

## Name an Image Repository Ahead of a Push

A build's first push names its own target's repository in the build's workspace. To place an image
name in a workspace before anything is pushed, REST `POST /v1/create_image_repository`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "reports"}
```

- MCP: `create_image_repository`. Repeating it for the same workspace answers the same route; a
  name another workspace holds is `conflict`.
- To list your image repositories, their workspace and tags, REST
  `GET /v1/list_image_repositories?customer_id=<customer id>&workspace_id=<workspace id>`
  (MCP `list_image_repositories`). A repository whose workspace cannot be read is listed with
  `tags_unavailable`.
- Everything Ship → Packages shows is `GET /v1/list_packages?customer_id=<customer id>` (MCP
  `list_packages`).

## Limits

- One image layer may be up to 4 GiB by default (the lake's `LAKE_MAX_REGISTRY_BLOB_BYTES`).
- A push counts against your account's storage limit; past it the registry answers `507 DENIED`
  and existing images stay readable ([Usage and Metering](/docs/usage)).
- A build pushes and reads only image repositories of its own workspace; it cannot use another
  workspace's image as a base or cache.
- Build progress topics are kept for 24 hours and up to 5,000 events.
- Deleting a workspace deletes its images; pulls of them then answer 404 ([Workspaces](/docs/workspaces)).

## Permissions

| Step | Needs |
|---|---|
| See packages, builds and logs | `read` on the account that owns the workspace |
| Build an image, build an app, cancel a build, name an image repository | `write` on the account |
| Pull a private image | a key with `registry_pull`, held by someone who may read the account |
| Publish npm or Python packages | the lake operator's administrator credential |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| `invalid_input` at `/workspace_id` | The workspace is missing, unknown, or not one of the account's. | Use the repository's own workspace. |
| A build stays queued | The operator has not picked it up. | Read its **Live log** and messages; ask the lake's operator if nothing arrives. |
| A build fails at fetching | The revision or path does not exist in the repository. | Check the revision and `context`. |
| `403 DENIED` from the registry | The credential is valid but not for this image: another account's image, or another workspace's image for a build. | Pull with a key of an account that may read the image. |
| `404 NAME_UNKNOWN` on a push | The image name is not routed to any workspace. | Push from a build, or call `create_image_repository` first. |
| `401` from `docker pull` | No login, or the key expired. | `docker login <lake host> -u lake` with a fresh pull key. |
| `503 UNAVAILABLE` with Retry-After | The workspace holding the image is not ready, or too many pulls are running. | Retry after the given time. |
| `conflict` naming an image repository | Another workspace already holds that name. | Choose another name. |

## Related Pages

- [Apps](/docs/apps): run an image, and build an app's own.
- [Repositories](/docs/repositories): the source a build reads.
- [Deployments and Environments](/docs/deployments): images deployed with `liquid deploy`.
- [Streams](/docs/streams): a build's progress topic.
- [Tokens and Keys](/docs/tokens): make and revoke a pull key.
- [Usage and Metering](/docs/usage): image storage in your usage.
