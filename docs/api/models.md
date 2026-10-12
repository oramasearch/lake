# Models and datasets

**Status: implemented in lake #7.** Models and datasets are ordinary lake repositories. Large
files, cards, metadata, history, named refs, and immutable revisions all use the customer liquid
store. Chunk identity is shared across revisions and repositories owned by the same customer, so
storage metering counts a chunk once even when several model or dataset versions reference it.

## Storage and transfer

The liquid sync routes in [`sync.md`](sync.md) are the native write and read path. Push negotiates
missing objects before transfer, pull requests only missing objects, and repeating a completed
pull transfers zero content bytes. Reference publication uses the repository policy object and
the same signed audit path as every other capability.

`get_model` and `get_dataset` return `storage` with logical, unique, and deduplicated byte totals,
the ratio saved, and the same values per revision. Each `siblings` entry includes its logical
`size`, locally unique `storedSize`, and `deduplicatedBytes`. Storage quotas are checked at the
customer liquid store and already-present chunks do not consume quota again.

The recorded two-revision fixture presents 4 GiB of logical weights. It writes repeated sparse
blocks, moves one populated block in the second revision, and measures a 0.999712 deduplication
ratio from the actual pack bytes. Its second push sends fewer bytes than its first push, the next
pull receives zero bytes, and the source and destination trees match.

## `/v1` capabilities

All capabilities use the repository policy object. Read capabilities allow anonymous access only
when the repository is public; action capabilities require repository `write` access. They are
registered once and therefore appear in `/v1/openapi.json` and `POST /mcp` automatically.

| Capability | Input | Result |
|---|---|---|
| `get_model`, repository `get_dataset` | `id`, `revision` | Repository identity, immutable `sha`, visibility, card, files, storage totals, and provenance |
| workspace `get_dataset` | `customer_id`, `workspace_id`, `id` as `<database>/<dataset>` (no `revision`) | Workspace dataset schema; an ApplicationAgent task credential supplies the customer implicitly |
| workspace `get_model` | `customer_id`, `workspace_id`, `id` as the model id Ship → Models shows (no `revision`) | The workspace's trained model: measures, feature columns, published state. The same record `workspace_query` `get_model` answers, so the name means one thing on both surfaces (lake#450, [naming.md](naming.md)); an unknown id is `not_found` at `/id` |
| `resolve_model_file`, `resolve_dataset_file` | `id`, `revision`, `path` | Exact bytes plus blob, size, and immutable revision metadata |
| `prepare_repository_upload` | `id`, `files[{path,size}]` | `regular` for files through 5 MiB, otherwise `lfs` |
| `stage_repository_file` | `id`, SHA-256 `oid`, `size`, binary body or base64 `content` | Unique stored bytes after digest and size verification |
| `commit_repository_files` | `id`, `revision?`, `message?`, `operations[]` | New immutable `commitOid`; operations are `file`, `lfsFile`, `deletedFile`, or `deletedFolder` |
| `attach_model_provenance` | model `id` and `revision`, dataset repository/revision, base64 liquid model and run attestations | Verified immutable dataset-to-model chain |
| `verify_model_provenance` | model `id`, `revision` | Chain fields, `verified`, and a refusal reason when verification fails |

The commit action writes regular bytes directly, consumes each staged LFS object once, applies
deletions, cuts one liquid change, and updates `main`. Read revisions may be named refs or
`blake3:` change digests. Upload preparation and commits use Lake tokens and never place a token
in a log or response.

## A workspace model: versions, training and serving (lake#599)

A model is a learned function. The workspace engine keeps one record per training, each with its
own id, the id `get_model`, `expose_model` and `predict` take. The web app groups the records of one
model, the same model name from the same pipeline of the same project (or the same experiment when
no pipeline is named), as its **versions**; a pipeline may hold several models, one per model step.
Grouping is the web app's (`frontend/src/pages/train/models.ts`); no capability changes for it.

- **Train → Models** is about training: each model's versions and held-out scores over time,
  comparing versions, what each learned from (`get_run_report`), its pipeline steps (`get_graph`),
  retraining (`run_pipeline` with the pipeline and `model_name`) and promoting a version to Ship.
- **Ship → Models** is about serving: publishing (`expose_model`, `unexpose_model`), the `predict`
  call and a try-it panel, the keys scoped to `predict`, where answers go, and monitoring from
  `model_traffic` ([workspaces.md](workspaces.md),
  [../serving/telemetry.md](../serving/telemetry.md#the-per-model-prediction-series)). A model a
  workspace methodology trained (`python:W-…`) is served by its own code once published, and
  `predict` answers one prediction per row from it ([methodologies.md](methodologies.md#serving-its-model), lake#690).

A model repository (the routes above) keeps model files with their history. Lake does not record
which repositories hold a model, so Train → Models does not list them; a file in one becomes a
workspace model when it is brought (below).

## A standalone model (lake#654)

A standalone model is made on its own, not as one step of a project's pipeline. Train → Models'
**New model** makes one, and four named engine capabilities (zetta, `docs/ml/standalone-models.md`)
plus one Lake capability carry it. Each takes `customer_id` and `workspace_id`, needs `write` on the
account like `workspace_act`, and is audited in the workspace's own chain.

| Capability | Input | What it does |
|---|---|---|
| `train_model` | `name`, `methodology` (an id from `list_methodologies`), `dataset_path` (`<database>/<name>` or `<database>.<name>`), `target` (required when the methodology's `label` input is), `entity_column?`, `holdout_fraction?` | Trains it: answers the `run_id`; the model appears in `list_models` when the run finishes |
| `retrain_model` | `model_id` (any version), and optionally a new `dataset_path`, `target`, `entity_column` or `holdout_fraction` | A new version. A standalone model keeps the new settings for its next retrain; a project's model uses them for this run only |
| `register_model` | `weights` (the file's contents), `name` or `version_of`, `kind?`, `metrics?` (the figures measured where it was trained), `source?` | Brings a model trained elsewhere so `score`, `explain`, `expose_model` and `predict` take it like a trained one |
| `load_drive_model` (Lake's) | `drive_id`, `id` (the file), `name` or `version_of`, `kind?`, `metrics?` | Reads the file as the caller (`read_drive_file` or `download_drive_file`) and sends it to `register_model`, as `load_drive_dataset` loads a table. Any drive kind: a workspace-files drive's entry id, a repository or storage drive's path, or a virtual drive's shortcut to a file, followed once ([drives.md](drives.md#a-drive-file-as-a-model-lake654)). At most 16 MiB |
| `move_model_to_project` | `model_id`, `project_name?` (default the model's name) | Makes it a project in Design → Projects, with its pipeline and every version |

- **Why the engine keeps a project for it.** Every methodology trains through a project and a
  pipeline, and the run and the model record them. So the engine gives a standalone model an
  internal project of its own, which `list_projects` never lists. Lake never shows it: `list_models`
  marks each version `standalone: true`, and the pages say "trained on its own" or "brought from a
  file" where a project's model names its pipeline and project.
- **What can be brought.** The engine scores, serves and explains what it reads natively: a
  gradient-boosted tree model in its own JSON weights format (`objective`, `base_score`,
  `feature_names`, `trees`), the format every model trained in Lake is kept in. Any other file is
  `not_supported` and nothing is kept. A model repository's file is read with
  `resolve_model_file` and sent as `weights`.
- **Delete.** `delete_model` deletes one version. Deleting a standalone model's last version also
  removes its internal project (`removed.standalone_project: 1`); its runs are kept.
- **An older engine.** A workspace whose engine predates these capabilities answers each with
  `not_supported` ("this workspace's engine does not have train_model yet; it needs a newer
  Zetta"), and nothing changes. This holds for every allow-listed engine capability.

## Deleting from a workspace

`delete_dataset`, `delete_model`, `delete_run` and `delete_index` (lake#645) delete one dataset,
trained model, run or search index from the workspace's engine. Each takes `customer_id`,
`workspace_id`, `id` and `dry_run?`, needs `write` on the account, like `workspace_act`, and is
recorded in the workspace's own audit chain (`workspace.delete`, or `workspace.delete.plan` for a
dry run, naming the capability, the id and what was removed).

| Capability | `id` | Engine input | What goes, and what is kept |
|---|---|---|---|
| `delete_dataset` | `<database>/<dataset>`, as `list_datasets` returns it | `dataset`, `database` | the table, its rows and partitions, and every search index on it; refused (`engine_error`, naming them) while a pending or running run or a running experiment reads it, its dry run too (zetta#1258) |
| `delete_model` | the model id (`model_id` is accepted too) | `model_id` | the trained model, no longer served if it was published; the run that trained it is kept. A standalone model's last version takes its internal project with it (`removed.standalone_project`) |
| `delete_run` | the run id (`run_id` too) | `run_id` | a finished run and its report; refused while pending or running; the models it trained are kept and named |
| `delete_index` | the index id (`index_id` too) | `index_id` | the index, no longer served if it was published; the dataset is kept |

The answer is the engine's (`removed`, and `kept` where something stays) with `id`, `dry_run` and
`deleted`. `dry_run: true` deletes nothing and answers what would go. An unknown id is
`not_found` at `/id`. The engine capabilities arrive with zetta#1257; a workspace whose engine is
older answers `501 not_supported`, saying so, and nothing is deleted. The refusal of a dataset a run
or an experiment is still reading arrives with zetta#1258; an engine before it deletes the dataset
without that check. Overview: [deletion.md](deletion.md).

## Training provenance

Training provenance uses two canonical libliquid attestations, not a Lake-specific envelope:

- `model_provenance/1` has the model revision as its subject and attributes for the immutable
  training-dataset digest and code revision.
- `slsa_provenance/1` has the same model revision as its subject and records parameters, the
  environment digest, runner identity, builder identity, and invocation ID.

Attachment decodes both canonical records, invokes libliquid's attestation verifier, requires
both signatures and profiles to be valid, requires both subjects to equal the selected immutable
model revision, and requires the dataset attribute to equal the resolved dataset digest. Only
then are the attestations stored and indexed against that model revision. Reads and
`verify_model_provenance` re-run verification and also require the referenced dataset revision to
remain available. Tampered bytes, a changed subject, a different dataset digest, an invalid
profile, or a missing dataset revision are refused. These are the same attestation primitives and
signature checks used by `liquid attest verify`; Lake retains both signed records by digest.

The model JSON includes `training_provenance`, and the server-rendered model page displays the
dataset and code revisions, run identity, parameters, environment digest, attestation digests,
verification state, and failure reason.

## Hugging Face-compatible HTTP

Both `namespace/name` identifiers and repository UUIDs are accepted where shown. The `models`
and `datasets` URL families map to the same repository and liquid storage model. A named revision
is resolved to an immutable liquid change digest before response metadata is returned.

| Route | Supported behavior |
|---|---|
| `POST /api/repos/create` | Creates a model or dataset repository for the named Lake customer, in the workspace named by the body's `workspace_id` or the `X-Lake-Workspace-Id` header (lake#315; `HfApi(headers={"X-Lake-Workspace-Id": ...})`, since `create_repo` cannot add body fields). Without one it is refused |
| `POST /api/validate-yaml` | Accepts the client compatibility check; Lake does not interpret card YAML |
| `GET /api/models/:namespace/:name[/revision/:revision]` | Model info, files, sizes, immutable revision, storage, and provenance |
| `GET /api/datasets/:namespace/:name[/revision/:revision]` | Dataset info, files, sizes, immutable revision, and storage |
| `GET /api/{models,datasets}/:namespace/:name/tree/:revision` | Recursive file listing used by `list_repo_files` and snapshots |
| `HEAD`, `GET /:namespace/:name/resolve/:revision/*file` | Model file metadata or exact bytes with `ETag` and immutable `X-Repo-Commit` |
| `HEAD`, `GET /datasets/:namespace/:name/resolve/:revision/*file` | Dataset file metadata or exact bytes |
| `POST /api/{models,datasets}/:namespace/:name/preupload/:revision` | Chooses regular or basic-LFS upload mode; reads only `files` from the body, so fields a client adds (`gitIgnore` from `huggingface_hub` 0.33.4 to 1.24.0) are ignored |
| `POST /api/{models,datasets}/:namespace/:name/commit/main` | Applies Hub NDJSON file, LFS-file, and delete operations as one liquid revision |
| `POST /:namespace/:name.git/info/lfs/objects/batch` | Returns a short-lived HMAC-authenticated basic-LFS upload URL |
| `PUT /api/lfs/:repository/:oid?...` | Stages one SHA-256 and size-verified object; the signed URL carries no Lake token |

This is the subset exercised by `huggingface_hub` 1.30.0 for `create_repo`, `upload_file`,
`upload_folder`, `list_repo_files`, `model_info`, `hf_hub_download`, and `snapshot_download` for
models and datasets. Named and immutable-digest revisions work for metadata, listing, and file
downloads. The committed integration test runs those calls with the real Python client when the
package is installed; otherwise it reports a clear skip, while the direct C++ HTTP test always
covers the same route families, including basic LFS.

The adapter does not provide Hub search, collections, discussions, pull requests, repository
move/delete APIs, branch-management or non-`main` commit APIs, Xet, multipart LFS, Git smart HTTP,
card-YAML semantics, or partial-range resolve downloads. One basic-LFS PUT is limited by the
server's 64 MiB request body limit. Native liquid sync is the supported route for larger resumable
model and dataset transfers.

## Verification

- `ModelSync.MultiGigabyteRevisionsDeduplicateAndResumePull` uses a process-unique temporary
  repository root so concurrent test processes cannot share fixture state, and proves measured
  deduplication and repeat-pull resumption with a 4 GiB logical two-revision model.
- `ModelProvenance.SignedDatasetChainVerifiesAndTamperingIsRefused` proves a signed
  dataset-to-model chain and rejects altered attestation bytes.
- `HuggingFaceHttp.CommonModelAndDatasetCallsUseRepositoryCapabilities` covers the supported HTTP
  requests directly, including LFS and the model page.
- `HuggingFaceClient.CommonFlowsRunWhenPythonClientIsInstalled` covers the listed client calls
  with the installed Python package or emits the required skip reason.
