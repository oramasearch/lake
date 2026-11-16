# The liquid CLI

`liquid` is the command line for Lake's repositories. It captures your writes as you save, names
them as changes, and pushes and pulls them to a repository on Lake. It also opens and approves
reviews, lists and resolves conflicts, and shows your identity for sign-up. This page covers what
you need to work with Lake; liquid's own reference (`liquid <command> --help`) lists every command
and flag.

## Concepts

| Term | What it is |
|---|---|
| **Drip** | One captured write to disk. Drips are automatic: liquid records them as you save. |
| **Change** | A named group of drips, made with `liquid cut "<message>"`. Changes are what you review, push and release. |
| **Reference** | A name pointing at a change, such as `main`. |
| **Checkout** | A local copy of a repository, made by `liquid clone` or `liquid init`. `push`, `pull` and most commands run inside one. |
| **Identity** | Your device's TAAP identity: a key pair whose private half never leaves the device. Every drip and change is signed with it. |
| **Repository URL** | `<lake origin>/v1/repositories/<repository id>`, the address every command that talks to Lake takes. The repository's **Clone** dialog shows it. |
| **`LIQUID_TOKEN`** | The environment variable liquid reads its Lake token from. It is sent as the bearer credential and never printed or put in a URL. |

## Install liquid

The **Sign Up** page links the supported build instructions
([docs/build/local.md](https://github.com/oramasearch/liquid/blob/main/docs/build/local.md) in the
`oramasearch/liquid` repository, which needs access) and shows these commands:

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja -DLIQUID_BUILD_TESTS=OFF
cmake --build build -j 6
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

Check it with `liquid version`.

## Your Identity

liquid creates your identity the first time it needs one.

```sh
liquid identity show
liquid identity show --document
```

The second form prints only the public identity document (base64url), which Lake's own sign-up
asks for. The private key stays on your device. `liquid identity rotate` replaces the signing key
with a linked successor, optionally moving it to a FIDO2 or PIV hardware key
(`--to fido2`, `--to piv:9c`).

## Make a Token for liquid

liquid authenticates to Lake with a token, not a password.

1. Open the account menu → **Tokens** → **New token**.
2. Name it, and in **Capability scopes, comma separated** press **Use these** beside the
   *liquid and liquify* list, which fills in
   `refs,find_missing,push,pull,upload_chunks,read_chunk,list_reviews,create_review,update_review,list_review_comments,create_review_comment,map_review_anchors,list_branch_protections`: clone, pull and push, and the reviews liquify opens and comments on.
3. Create it and copy the secret; it is shown once.
4. Export it in your shell: `export LIQUID_TOKEN=<the token>`.

- REST: `POST /v1/create_token` with
  `{"name": "liquid on my laptop", "scopes": ["refs", "find_missing", "push", "pull", "upload_chunks", "read_chunk"]}`.
- MCP: `create_token` with the same input.

**Why those scopes.** A token can call only the capabilities its scopes name, on the objects your
own grants reach. `pull` and `read_chunk` read history and content; `refs` lists references;
`find_missing` and `upload_chunks` move only the objects the other side lacks; `push` publishes.
A pull-only token can leave out `push` and `upload_chunks`. A token never holds more than you do:
pushing still needs `write` on the repository, and branch protection still applies. Keys and their
lifetimes are covered in [Tokens and Keys](/docs/tokens).

## Clone a Repository

Open the repository on Lake and press **Clone**. The dialog's first block is:

```sh
liquid clone https://lake.example.com/v1/repositories/<repository id> analytics
cd analytics
```

`clone` creates the directory, pulls the history and writes every file. If you leave out the
directory, liquid names it after the last part of the URL. The destination must be new or empty.

## Work and Name Your Changes

Inside the checkout, edit files as you normally would; liquid captures each save.

```sh
liquid status
liquid cut "Add the churn notebook"
liquid log
liquid diff <change a> <change b>
```

- `status` shows the checkout's state. On macOS and Linux it starts liquid's background capture
  daemon on first use; `liquid daemon --shutdown` stops it, and `liquid pause` / `liquid resume`
  stop and restart capture.
- `cut` names everything captured since the last change as one change and prints its id.
- `log` lists named changes; `log --ops` lists the signed drips.
- `diff` compares two changes or references, following files across moves.
- `move <from> <to>` and `rm <path>` record a move or removal explicitly.
- `liquid search "<text>"` searches the checkout's paths and contents.

## Push and Pull

```sh
liquid push https://lake.example.com/v1/repositories/<repository id>
liquid pull https://lake.example.com/v1/repositories/<repository id>
liquid refs
```

- `push` sends your changes and only the objects Lake does not have yet. The reply lists every
  reference as applied or refused, with the reason for a refusal.
- `push --atomic` refuses every reference update when any one is refused, so a push lands whole or
  not at all.
- `pull` brings in what others pushed since your last pull.
- `refs` lists the checkout's references; `liquid refs --json` gives the id of `main`.

Push and pull need the network. Every other command works offline against the checkout. With
`LIQUID_OFFLINE=1` set, a command that needs the network stops before it starts and exits 69.

## Reviews from the Command Line

`liquid review` works on Lake's reviews for the repository named by `--lake <repository URL>`:

```sh
liquid review list --lake https://lake.example.com/v1/repositories/<repository id> --state open
liquid review open <change id> --lake https://lake.example.com/v1/repositories/<repository id>
liquid review comment <review id> src/parse.cpp:42@<change id> "Handle an empty file here." --lake https://lake.example.com/v1/repositories/<repository id>
liquid review approve <review id> --lake https://lake.example.com/v1/repositories/<repository id>
```

`show`, `close`, `watch` and `protections` work the same way, and `--json` writes stable JSON. A
comment's anchor is `<path>:<line>[:<column>]@<change>`. The reviews appear on the change's page in
the web app ([Reviews, Issues and Releases](/docs/repository-collaboration)).

## Conflicts

When two edits to the same thing cannot be merged safely, liquid keeps both values.

```sh
liquid conflicts
liquid conflict show <path>
liquid conflict resolve <path>
```

The same conflicts show on the repository's **Conflicts** page, where they can also be resolved.

## Other Commands You May Meet

| Command | What it does |
|---|---|
| `liquid init [path]` | Make a new local repository from the files in a directory. To publish it to Lake, create the repository on Lake first, then push to its URL. |
| `liquid history <path>` | A file's history across moves; `--restore <change>` brings an old version back as a new write. |
| `liquid blame <path>` | Which change last wrote each line. |
| `liquid import git <path>` | Import a Git repository's history into liquid. |
| `liquid export git <path>` | Write the history as a Git repository. |
| `liquid drive sync <folder> <remote>` | Keep a local folder synchronized with a repository, as the Drive view does. |
| `liquid wrap <build command>` / `liquid build` | Run a build as cached, attested actions; the results appear on the repository's **Artifacts** tab after a push. |

## Limits

- One sync request body is at most 64 MiB.
- When your account's storage limit is reached, pushes are refused (HTTP 429, `busy`) and
  everything already stored stays readable ([Usage and Metering](/docs/usage)).
- The background capture daemon runs on macOS and Linux.
- Some build commands (`wrap`, `build`, `test`) need FUSE on Linux or macFUSE on macOS.

## Permissions

| To | You need |
|---|---|
| Clone or pull | a token with `refs`, `find_missing`, `pull` and `read_chunk`, and `read` on the repository; sync always needs a token, even for a public repository |
| Push | also `push` and `upload_chunks`, and `write` on the repository; protected references also follow their rules |
| Open, comment on or approve a review | `write` on the repository |

## Troubleshooting

| You see | Why, and what to do |
|---|---|
| `unauthorized` (401) on push or pull | `LIQUID_TOKEN` is not set, mistyped, expired or revoked, or comes from another Lake. Make a new token on this Lake and export it. |
| `unauthorized` (403) | The token lacks one of the sync scopes, or you lack `read` or `write` on the repository. |
| A reference reported as refused | Branch protection turned it away; the reason names the rule ([Branch Protection](/docs/repository-settings#branch-protection)). |
| `conflict` (409) | The repository's identity differs from the checkout's: you are pushing a different repository to this URL. |
| `locked` (423) | Another push holds the reference; push again. |
| `busy` (429) | The storage limit is reached, or Lake is busy. |
| `unavailable` (503) | The repository's store is being handed over; push again. |
| `needs the network for …`, exit 69 | `LIQUID_OFFLINE=1` is set. Unset it to push or pull. |
| The Clone dialog shows no commands | This Lake has no external origin configured; the operator must set one. |

## Related Pages

- [Repositories](/docs/repositories)
- [Reviews, Issues and Releases](/docs/repository-collaboration)
- [Branch Protection, Webhooks, Import and Export](/docs/repository-settings)
- [Tokens and Keys](/docs/tokens)
- [Your Account and Sign-In](/docs/account)
- [Developer](/docs/developer)
