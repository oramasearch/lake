# lake as Orama's cloud: information architecture and navigation

**Status: partly implemented (2026-09-25).** The regrouped sidebar, the account menu, route-keyed navigation, the new home page, Files as the first repository tab, and the cross-repository Deployments page are built; see [`../ui/front-door.md`](../ui/front-door.md). Design, Train and Ship over per-customer Zetta workspaces are built too ([`../api/workspaces.md`](../api/workspaces.md)); Packages and the Organization section are not. This page
describes what lake's navigation and
home page become once lake is Orama's cloud offering. What lake serves today is in
[`../ui/front-door.md`](../ui/front-door.md) and stays accurate until the build issues listed at
the end land.

The static mock is [`ui/mocks/cloud/`](ui/mocks/cloud/README.md).

## What changes, and why

lake today presents itself as a place to host liquid repositories: "Keep your code and files
together", with a three-step path from account to `liquid push`. Orama now describes what it does
in one line: **we help companies design, train, and ship AI**
(Orama's website). lake is where a customer does that in
Orama's cloud, so its navigation should follow the same three verbs.

Four decisions shape the design (2026-09-25):

| Decision | Consequence for the navigation |
|---|---|
| The product is called **lake**, served at **lake.live**, linked from orama.com. Today the only deployment is `lake.example.com` on AKS; lake.live is the target host | The brand in the sidebar stays `lake`. Page titles keep the `lake — …` form |
| lake is the **cloud** offering, and can also run **on-prem** for enterprise customers. **Orama Studio is a separate on-prem enterprise product** | lake does not link out to Studio. Everything a cloud customer designs, trains and ships is a lake page. The same navigation serves an on-prem lake |
| **Repositories are the central object.** A liquid repository is a shared, multiplayer place for any data (files, objects, datasets, models, code), closer to a shared drive folder than a code repository | Repositories sit at the top of the navigation. Design, Train and Ship are views of what lives in them, never a separate store |
| Sign-in is **lake's own identity** (passwords, passkeys, TOTP, OAuth, SSO, SCIM) | No change to the session model or `context_for` |

The fourth already matches the system: `models.md` and `drive.md` treat models, datasets and
folders as ordinary repositories, and the design leans on that.

## The sections

| Section | What it is for | Pages | Built today |
|---|---|---|---|
| **Home** | Where the viewer is, and the next step | `/` | Built, with copy about liquid push. Rewritten |
| **Repositories** | Every repository the viewer can open, of every kind, and what has been shared with them | `/repositories`, `/shared-with-me`, then per repository: Files, Changes, Issues, Releases, Artifacts, Deployments, Access | Built. The repository home opens on its code tree, not its files |
| **Search** | One search across everything the viewer can read | `/search` | Built |
| **Design** | Projects: the business question, the decisions it drives, and the return it is measured against | `/projects` | **Not built.** Proposed below as a repository kind |
| **Train** | Datasets, models, and the runs that connect them | `/datasets`, `/models` | Model and dataset pages and training provenance built per repository; **no list pages** |
| **Ship** | What is running, where, and what it serves | `/deployments`, `/packages`, `/applications` | Environments, deployments, logs and domains built **per repository**; package registries built as protocols **without pages**; applications built for operators only |
| **Organization** | People, access, secrets, usage and audit for the customer | `/organizations`, `/sso`, `/secret-mounts`, `/usage`, `/audit` | Built, but mostly under Administration and visible only to operators |
| **Developer** | API and MCP | `/v1/openapi.json`, `POST /mcp` | Built |

Account pages (Settings, Sessions, Passkeys, Tokens) move out of the sidebar into an account menu
in the top bar. They are about the person, not the work.

### Repositories come first

Repositories are listed before Design, Train and Ship because every one of those sections reads
repositories. A dataset is a repository with dataset metadata. A model is a repository with a model
card and revisions. A deployment is an environment of a repository. A project (below) would be a
repository too. Putting the object first and the verbs after it says that plainly, and it keeps the
navigation honest: there is no second place where data lives.

Inside a repository, the first tab becomes **Files** (the drive view, `drive.md`) instead of
**Code** (the tree view). Most repositories a cloud customer creates hold data, documents and
models, and the drive view is written for people who work with folders. The code tree stays one
click away. The per-repository tab labelled **Platform** today is renamed **Deployments**, which
is what it shows.

Each repository row in the list gains a **kind**: files, dataset, model, code, or project. The kind
is derived, never a separate store: model and dataset metadata already mark those repositories, and
anything else is files unless its content is predominantly source.

### Design: projects

Design is the one section with no implementation behind it. The proposal: **a project is a
repository** holding a project document. The document states the business question, the decisions
it drives, the owner of each decision, and the return the work is measured against. It links the
datasets, models and deployments that serve it.

Making a project a repository gives it history, review, sharing, multiplayer editing and access
control for free, and it keeps projects out of the Postgres control plane (by
design). The project page renders the document and the linked repositories. This needs its own
decision before any code is written; it is question 1 below.

### Train: datasets and models

`/datasets` and `/models` are new list pages over repositories that already carry dataset or model
metadata. Each row links to the existing model or dataset page. The model page already shows the
verified attestation chain from dataset and code revisions through the training run, so a
training run is reached from its model. A dedicated runs page waits until lake executes training
itself rather than recording its provenance.

### Ship: deployments, packages, applications

`/deployments` is a new list across every repository the viewer can read, built from the same
`list_environments` and `list_deployments` capabilities the per-repository pages call. `/packages`
is a new page listing what the OCI, npm and Python registries hold for the customer. `/applications`
exists for operators today; the proposal is to show a customer the applications their own
organization registered, through the same capability and policy.

## Navigation per viewer

| Viewer | Sidebar | Top bar |
|---|---|---|
| Signed out | Home, Public repositories | Sign in, Create account |
| Account | Home · Repositories, Shared with me, Search · **Design:** Projects · **Train:** Datasets, Models · **Ship:** Deployments, Packages, Applications · **Organization:** Members and teams, Secrets, Usage, Audit · **Developer:** API and MCP | Account menu (Settings, Sessions, Passkeys, Tokens, Sign out) |
| Operator | The account sidebar, plus **Administration:** Customers, Organizations, Applications, Metering, Audit | Account menu |

A section appears only when the viewer can open at least one page in it. On-prem, the operator is
the customer's own administrator, and the Administration section serves them unchanged.

**The active item is keyed on the route, not the page title.** `navigation_is_current` matches the
exact `lake — …` title strings of about forty pages today. Each new page would add another string
to match, and a changed title silently loses its highlight. The navigation should compare the
request path against a prefix list per item instead. This is the first build step because every
later one depends on it.

## Home

**Signed out.** The eyebrow names the product and the company: *lake by Orama*. The headline is
*Design, train, and ship AI on your own data.* The paragraph: *Keep your files, datasets and models
in shared repositories, train models on them, and put them into production, in Orama's cloud or
your own.* Actions: **Create account**, **Sign in**. Below, three short cards (Design, Train, Ship)
say what each section does. The liquid command line appears on the Ship card, as one way to push,
instead of as the headline journey.

**Signed in.** Recent repositories first, then one row per section with its counts (projects,
datasets, models, running deployments), each linking to its list. A new customer with nothing yet
sees the three cards with one action each: create a project, add a dataset, create a repository.

## Questions still open

1. **Is a project a repository?** Proposed above. The alternative is a control-plane record, which
   would add a table and a second place data lives.
2. **Where do the orama.com links point?** Straight to `https://lake.live/`, or through a stable
   `orama.com/cloud` redirect that survives a future host change.
3. **Do liquid's public repositories stay on lake.live?** Signed-out visitors today see Public
   repositories. For the cloud offering that may belong under a path such as `/explore`, or
   nowhere until liquid is open source.
4. **Orama Cloud v1 and v2.** What lake.live means for `app.orama.com` and the Railway stack, and
   how their customers move, is outside this design and needs its own decision.

## Build issues this design produces

Each lands as its own lake PR, in this order, once the design is approved:

1. Route-keyed active state for the navigation (replaces the title matcher).
2. The new sidebar grouping and the account menu, with the per-viewer rules above.
3. The new Home, signed out and signed in.
4. Repository kinds in the list, Files as the first repository tab, Platform renamed Deployments.
5. `/datasets` and `/models` list pages.
6. `/deployments` across repositories, and `/packages`.
7. Customer-visible `/applications`, members and teams, secrets, usage and audit under Organization.
8. Projects, after question 1 is decided.

A follow-up on Orama's website adds the lake.live link to the site header and footer.

## How to verify this page

It changes nothing that runs. Open the mock pages listed in
[`ui/mocks/cloud/README.md`](ui/mocks/cloud/README.md) and compare each section and viewer against
the tables above.
