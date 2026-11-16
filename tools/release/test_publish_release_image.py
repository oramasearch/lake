#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Checks of publish-release-image.py (lake#994) with Git, GitHub and the registry replaced.

run: python3 -I -m unittest discover -s tools/release -p 'test_*.py'
"""
import importlib.util
import json
import os
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "publish_release_image", os.path.join(HERE, "publish-release-image.py"))
publish = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish)

COMMIT = "2e6024cf55e570af546c0359691881f13814074b"
OTHER = "0" * 40
DIGEST = "sha256:" + "a" * 64
MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"


class Registry:
    """The build tag's manifest and config; `release_digest` is what the release tag names."""

    def __init__(self, revision=COMMIT, release_digest=None):
        self.revision = revision
        self.release_digest = release_digest
        self.pushed = []

    def fetch(self, token, path, accept=publish.MANIFEST_TYPES):
        if path == "manifests/20261114a":
            body = json.dumps({"config": {"digest": "sha256:config"}}).encode()
            return body, MANIFEST_TYPE, DIGEST
        if path == "blobs/sha256:config":
            labels = {publish.REVISION_LABEL: self.revision}
            return json.dumps({"config": {"Labels": labels}}).encode(), "application/json", None
        if path.startswith("manifests/v"):
            if self.release_digest is None:
                return None, None, None
            return b"{}", MANIFEST_TYPE, self.release_digest
        raise AssertionError(path)


def github(merged_by="homer6", head="dev", base="main", merge_commit=COMMIT, reviews=()):
    pull = {"number": 1269, "merge_commit_sha": merge_commit, "merged_at": "2026-11-14T00:00:00Z",
            "base": {"ref": base}, "head": {"ref": head}, "merged_by": {"login": merged_by}}

    def run(*command):
        if command[:2] == ("git", "ls-remote"):
            return f"{COMMIT}\trefs/tags/v0.9.0"
        if command[:2] == ("git", "fetch"):
            return ""
        if command[:2] == ("gh", "api"):
            path = command[-1]
            if path.endswith(f"commits/{COMMIT}/pulls"):
                return json.dumps([pull])
            if path.endswith("pulls/1269"):
                return json.dumps(pull)
            if path.endswith("pulls/1269/reviews"):
                return json.dumps(list(reviews))
        raise AssertionError(command)

    return run


class PublishReleaseImage(unittest.TestCase):
    def publish(self, registry, run, args=("20261114a", "v0.9.0")):
        ancestor = mock.Mock(returncode=0)
        with mock.patch.object(publish, "run", run), \
                mock.patch.object(publish, "fetch", registry.fetch), \
                mock.patch.object(publish, "registry_token", lambda scope: "token-" + scope), \
                mock.patch.object(publish, "repository_is_private", lambda: True), \
                mock.patch.object(publish.subprocess, "run", return_value=ancestor), \
                mock.patch.object(publish.urllib.request, "urlopen") as urlopen:
            publish.main(list(args))
            return urlopen

    def refused(self, registry, run, args=("20261114a", "v0.9.0")):
        with self.assertRaises(SystemExit) as raised:
            self.publish(registry, run, args)
        return str(raised.exception.code)

    def test_a_release_name_is_never_a_build_tag(self):
        self.assertIn("not a build tag", self.refused(Registry(), github(), ("v0.8.1", "v0.9.0")))
        self.assertIn("not a release version",
                      self.refused(Registry(), github(), ("20261114a", "0.9.0")))

    def test_an_existing_release_tag_is_never_written_again(self):
        for existing in (DIGEST, "sha256:" + "b" * 64):
            message = self.refused(Registry(release_digest=existing), github())
            self.assertIn("already exists", message)

    def test_the_image_must_come_from_the_tagged_commit(self):
        self.assertIn("was built from", self.refused(Registry(revision=OTHER), github()))
        self.assertIn("unknown revision", self.refused(Registry(revision=""), github()))

    def test_the_source_must_be_an_approved_release_pull_request(self):
        self.assertIn("neither merged nor approved", self.refused(Registry(), github("someone")))
        self.assertIn("release pull request", self.refused(Registry(), github(head="feature")))
        self.assertIn("release pull request", self.refused(Registry(), github(base="dev")))
        # Merged by someone else but approved by an approver in a review is approved.
        approved = github("someone", reviews=[{"user": {"login": "homer6"}, "state": "APPROVED"}])
        self.assertIn("dry run", self.dry_run(approved))

    def dry_run(self, run):
        printed = []
        with mock.patch("builtins.print", lambda *a: printed.append(" ".join(map(str, a)))):
            urlopen = self.publish(Registry(), run, ("20261114a", "v0.9.0", "--dry-run"))
        urlopen.assert_not_called()
        return "\n".join(printed)

    def test_a_dry_run_writes_nothing(self):
        self.assertIn("would name it", self.dry_run(github()))

    def test_a_tag_only_on_this_machine_does_not_count(self):
        def no_tag(*command):
            if command[:2] == ("git", "ls-remote"):
                return ""
            return github()(*command)
        self.assertIn("not on origin", self.refused(Registry(), no_tag))


if __name__ == "__main__":
    unittest.main()
