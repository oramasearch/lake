#!/usr/bin/env python3
"""Give a built lake image its release tag in Docker Hub, only from a tagged, approved source.

usage: DOCKER_CONFIG=<dir> python3 -I tools/release/publish-release-image.py <build-tag> <vX.Y.Z>
       [--dry-run]

e.g.   publish-release-image.py 20261114a v0.9.0

The release path of lake#994 (docs/deployment/release-images.md). The image is built once on the
cluster BuildKit under its build tag (`<YYYYMMDD><letter>`), deployed, and then named `vX.Y.Z`
through the registry API without rebuilding. Before anything is written, every check below must
pass; any failure stops the script with nothing pushed:

1. `vX.Y.Z` is a release version and the build tag is not one: a build never pushes a release name.
2. The Git tag `vX.Y.Z` exists on origin (a tag only on this machine does not count).
3. The tagged commit is approved: it is the merge commit of a merged pull request into `main` from
   `dev` (the release pull request), merged by, or approved in a review by, someone listed in
   `tools/release/approvers` next to this file. It is also an ancestor of `origin/main`.
4. The build tag's image was built from that commit: its `org.opencontainers.image.revision` label
   (the Dockerfile's LAKE_BUILD_REVISION) is the tagged commit.
5. `oramasearch/lake:vX.Y.Z` does not exist yet. A release tag is never moved, and is never
   written again even with the same digest. An existing tag is reported and refused before any
   push credential is requested.
6. The repository is private: an anonymous Docker Hub request for it answers 404, before and after.

Only then is a push-scoped token requested and the manifest written under the release tag, and
the new tag read back: it must name the build tag's digest. Pull and push use separate tokens.
Reads the registry credential from DOCKER_CONFIG/config.json and never prints it. Needs `git` (run
from a lake checkout whose `origin` is oramasearch/lake) and an authenticated `gh`.

Docker Hub tag immutability for `v*` tags is a repository setting only a person can turn on; until
it is, this script is what keeps a release tag from being overwritten (human step in
docs/deployment/release-images.md).
"""
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

GITHUB_REPO = "oramasearch/lake"
IMAGE_REPO = "oramasearch/lake"
REGISTRY = "https://registry-1.docker.io"
RELEASE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
REVISION_LABEL = "org.opencontainers.image.revision"
MANIFEST_TYPES = ", ".join([
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.docker.distribution.manifest.v2+json",
])
INDEX_TYPES = (
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
)


def refuse(message):
    sys.exit("refused: " + message)


def run(*command):
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        refuse(f"{' '.join(command[:3])} failed: {done.stderr.strip()}")
    return done.stdout.strip()


def approvers():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "approvers")
    with open(path) as file:
        names = {line.split("#")[0].strip() for line in file}
    names.discard("")
    if not names:
        refuse(f"{path} lists no approver")
    return names


def tagged_commit(version):
    # The tag as origin has it, peeled to its commit: a local tag does not count.
    lines = run("git", "ls-remote", "--tags", "origin", f"refs/tags/{version}",
                f"refs/tags/{version}^{{}}").splitlines()
    refs = {}
    for line in lines:
        sha, _, ref = line.partition("\t")
        refs[ref] = sha
    commit = refs.get(f"refs/tags/{version}^{{}}") or refs.get(f"refs/tags/{version}")
    if not commit:
        refuse(f"the Git tag {version} is not on origin")
    return commit


def approved(commit, version):
    run("git", "fetch", "--quiet", "origin", "main")
    ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", commit, "FETCH_HEAD"])
    if ancestor.returncode != 0:
        refuse(f"{version} ({commit}) is not on origin/main")
    pulls = json.loads(run("gh", "api", f"repos/{GITHUB_REPO}/commits/{commit}/pulls"))
    release = [
        pull for pull in pulls
        if pull.get("merge_commit_sha") == commit and pull.get("merged_at")
        and pull["base"]["ref"] == "main" and pull["head"]["ref"] == "dev"
    ]
    if not release:
        refuse(f"{version} ({commit}) is not the merge commit of a release pull request "
               "from dev into main")
    pull = json.loads(run("gh", "api", f"repos/{GITHUB_REPO}/pulls/{release[0]['number']}"))
    reviews = json.loads(run("gh", "api", "--paginate",
                             f"repos/{GITHUB_REPO}/pulls/{pull['number']}/reviews"))
    allowed = approvers()
    merged_by = (pull.get("merged_by") or {}).get("login", "")
    approving = {review["user"]["login"] for review in reviews if review.get("state") == "APPROVED"}
    who = ({merged_by} | approving) & allowed
    if not who:
        refuse(f"release pull request #{pull['number']} was neither merged nor approved by an "
               f"approver in tools/release/approvers ({', '.join(sorted(allowed))})")
    return pull["number"], sorted(who)


def registry_token(scope):
    config_dir = os.environ.get("DOCKER_CONFIG")
    if not config_dir:
        refuse("DOCKER_CONFIG is not set")
    with open(os.path.join(config_dir, "config.json")) as file:
        config = json.load(file)
    auth = next((value["auth"] for key, value in config.get("auths", {}).items()
                 if "docker.io" in key and "auth" in value), None)
    if not auth:
        refuse("no docker.io credential in DOCKER_CONFIG")
    request = urllib.request.Request(
        "https://auth.docker.io/token?service=registry.docker.io"
        f"&scope=repository:{IMAGE_REPO}:{scope}",
        headers={"Authorization": "Basic " + auth})
    with urllib.request.urlopen(request) as response:
        return json.load(response)["token"]


def fetch(token, path, accept=MANIFEST_TYPES):
    request = urllib.request.Request(f"{REGISTRY}/v2/{IMAGE_REPO}/{path}",
                                     headers={"Authorization": "Bearer " + token,
                                              "Accept": accept})
    try:
        with urllib.request.urlopen(request) as response:
            return (response.read(), response.headers.get("Content-Type"),
                    response.headers.get("Docker-Content-Digest"))
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None, None, None
        raise


def image_revision(token, body, content_type):
    manifest = json.loads(body)
    if content_type in INDEX_TYPES or manifest.get("manifests"):
        platforms = [
            entry for entry in manifest.get("manifests", [])
            if entry.get("platform", {}).get("os") == "linux"
            and entry.get("platform", {}).get("architecture") == "amd64"
        ]
        if len(platforms) != 1:
            refuse("the build tag's index holds no single linux/amd64 image")
        body, content_type, _ = fetch(token, f"manifests/{platforms[0]['digest']}")
        manifest = json.loads(body)
    config_digest = manifest.get("config", {}).get("digest")
    if not config_digest:
        refuse("the build tag's manifest names no image config")
    config, _, _ = fetch(token, f"blobs/{config_digest}", accept="*/*")
    labels = (json.loads(config).get("config") or {}).get("Labels") or {}
    return labels.get(REVISION_LABEL, "")


def repository_is_private():
    request = urllib.request.Request(
        f"https://hub.docker.com/v2/repositories/{IMAGE_REPO}/", method="GET")
    try:
        with urllib.request.urlopen(request) as response:
            return response.status == 404
    except urllib.error.HTTPError as error:
        return error.code == 404


def main(argv):
    dry_run = "--dry-run" in argv
    args = [arg for arg in argv if arg != "--dry-run"]
    if len(args) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    build_tag, version = args
    if not RELEASE.match(version):
        refuse(f"{version} is not a release version vX.Y.Z")
    if RELEASE.match(build_tag) or build_tag in ("latest", "main", "dev"):
        refuse(f"{build_tag} is not a build tag; a release is named only from its build tag")

    commit = tagged_commit(version)
    number, who = approved(commit, version)
    print(f"{version} is {commit}, release pull request #{number} (approved by {', '.join(who)})")

    if not repository_is_private():
        refuse(f"{IMAGE_REPO} answers an anonymous request: it is not private")
    pull_token = registry_token("pull")
    body, content_type, digest = fetch(pull_token, f"manifests/{build_tag}")
    if body is None:
        refuse(f"{IMAGE_REPO}:{build_tag} not found")
    revision = image_revision(pull_token, body, content_type)
    if revision != commit:
        refuse(f"{IMAGE_REPO}:{build_tag} was built from {revision or 'an unknown revision'}, "
               f"not {version} ({commit})")
    _, _, existing = fetch(pull_token, f"manifests/{version}")
    if existing is not None:
        refuse(f"{IMAGE_REPO}:{version} already exists ({existing}); a release tag is never "
               "written twice")
    print(f"{IMAGE_REPO}:{build_tag} {digest} built from {commit}")
    if dry_run:
        print(f"dry run: would name it {IMAGE_REPO}:{version}")
        return

    push_token = registry_token("pull,push")
    put = urllib.request.Request(f"{REGISTRY}/v2/{IMAGE_REPO}/manifests/{version}", data=body,
                                 method="PUT",
                                 headers={"Authorization": "Bearer " + push_token,
                                          "Content-Type": content_type})
    with urllib.request.urlopen(put) as response:
        pushed = response.headers.get("Docker-Content-Digest")
    _, _, check = fetch(pull_token, f"manifests/{version}")
    print(f"{IMAGE_REPO}:{version} {check}")
    if not (check == digest == pushed):
        refuse(f"{IMAGE_REPO}:{version} names {check}, not {digest}")
    if not repository_is_private():
        refuse(f"{IMAGE_REPO} answers an anonymous request after the push: make it private now")
    print("same digest: True")


if __name__ == "__main__":
    main(sys.argv[1:])
