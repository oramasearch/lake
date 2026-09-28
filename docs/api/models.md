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
| workspace `get_dataset` | `customer_id`, `id` as `<database>/<dataset>` | Workspace dataset schema for ApplicationAgent; its task credential supplies the customer implicitly |
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
| `POST /api/repos/create` | Creates a model or dataset repository for the named Lake customer |
| `POST /api/validate-yaml` | Accepts the client compatibility check; Lake does not interpret card YAML |
| `GET /api/models/:namespace/:name[/revision/:revision]` | Model info, files, sizes, immutable revision, storage, and provenance |
| `GET /api/datasets/:namespace/:name[/revision/:revision]` | Dataset info, files, sizes, immutable revision, and storage |
| `GET /api/{models,datasets}/:namespace/:name/tree/:revision` | Recursive file listing used by `list_repo_files` and snapshots |
| `HEAD`, `GET /:namespace/:name/resolve/:revision/*file` | Model file metadata or exact bytes with `ETag` and immutable `X-Repo-Commit` |
| `HEAD`, `GET /datasets/:namespace/:name/resolve/:revision/*file` | Dataset file metadata or exact bytes |
| `POST /api/{models,datasets}/:namespace/:name/preupload/:revision` | Chooses regular or basic-LFS upload mode |
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
