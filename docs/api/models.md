# Models and datasets

**Status: implemented for lake #7.** Models and datasets are repositories, so model weights and
dataset files use the same content-addressed deduplication, refs, signed changes, authorization,
and push/pull protocol as source. Dataset versioning is repository history rather than a second
version counter.

## Model contract

A model repository contains `MODEL_CARD.md` or Hugging Face's conventional `README.md`. The
`get_model` capability accepts repository `id` and `revision`; it returns the card, file siblings,
repository revision, visibility, and training/build provenance. For every recorded build result,
the response requires verified `sbom/1` and `slsa_provenance/1` attestations. An incomplete result
returns `conflict` instead of presenting unverified provenance as complete.

`resolve_model_file` accepts `id`, `revision`, and `path`, authorizes repository `read`, and returns
the exact liquid blob bytes. Anonymous access is allowed only for a public model repository; user
credentials use the ordinary repository policy object.

## Hugging Face-compatible HTTP

| Route | Result |
|---|---|
| `GET /api/models/:id` | Model metadata with `id`, `modelId`, `sha`, `private`, `siblings`, card, and provenance |
| `GET /models/:id/resolve/:revision/*file` | Exact bytes for a file path, including nested paths |

The adapter calls `get_model` and `resolve_model_file`; it has no model-specific authorization
path. The recorded HTTP client test opens metadata, reads the card and provenance flag, then
downloads fixture weights through the standard resolve route.

## Verification

`Models.MetadataAndResolveUseRepositoryHistoryAndProvenance` covers the capabilities and both
required attestation profiles. `WebUiHttp.PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden`
records the compatible metadata and file-resolution requests against a real liquid repository.
