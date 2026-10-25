import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from scripts.repair_release_metadata import (
    CHECKSUMS_NAME,
    MANIFEST_NAME,
    Contract,
    prepare,
    verify_final,
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class RepairReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.original = self.root / "original"
        self.repaired = self.root / "repaired"
        self.original.mkdir()
        self.package_name = "FileFlow-v1.2.3-linux-x86_64.tar.gz"
        package_bytes = b"reviewed installer bytes"
        (self.original / self.package_name).write_bytes(package_bytes)
        package_sha = sha256(package_bytes)
        bad_segment = "untagged-reviewed-draft"
        manifest = {
            "schemaVersion": 1,
            "version": "1.2.3",
            "date": "2026-10-25",
            "artifacts": [{
                "name": self.package_name,
                "platform": "linux",
                "arch": "x86_64",
                "url": f"https://github.com/Owner/Repo/releases/download/{bad_segment}/{self.package_name}",
                "sha256": package_sha,
                "signing": {"status": "unsigned", "details": "No signature."},
                "notes": "Test package.",
                "knownIssues": ["Test limitation."],
            }],
        }
        manifest_bytes = (json.dumps(manifest, indent=2) + "\n").encode()
        (self.original / MANIFEST_NAME).write_bytes(manifest_bytes)
        manifest_sha = sha256(manifest_bytes)
        sums_bytes = f"{manifest_sha}  {MANIFEST_NAME}\n{package_sha}  {self.package_name}\n".encode()
        (self.original / CHECKSUMS_NAME).write_bytes(sums_bytes)
        self.contract = Contract(
            repository="Owner/Repo",
            release_id=42,
            tag="v1.2.3",
            version="1.2.3",
            commit="a" * 40,
            published_date="2026-10-25",
            bad_url_segment=bad_segment,
            packages={self.package_name: package_sha},
            original_manifest_sha256=manifest_sha,
            original_checksums_sha256=sha256(sums_bytes),
        )
        self.release = {
            "id": 42,
            "tag_name": "v1.2.3",
            "target_commitish": "a" * 40,
            "draft": False,
            "prerelease": False,
            "immutable": False,
            "published_at": "2026-10-25T12:00:00Z",
            "assets": [
                self.asset(1, self.package_name, package_sha, len(package_bytes)),
                self.asset(2, MANIFEST_NAME, manifest_sha, len(manifest_bytes)),
                self.asset(3, CHECKSUMS_NAME, sha256(sums_bytes), len(sums_bytes)),
            ],
        }
        self.tag_ref = {"ref": "refs/tags/v1.2.3", "object": {"type": "commit", "sha": "a" * 40}}
        self.release_path = self.write_json("release.json", self.release)
        self.tag_path = self.write_json("tag.json", self.tag_ref)

    def tearDown(self):
        self.temp.cleanup()

    def asset(self, asset_id, name, digest, size):
        return {
            "id": asset_id,
            "name": name,
            "size": size,
            "digest": f"sha256:{digest}",
            "browser_download_url": f"https://github.com/Owner/Repo/releases/download/v1.2.3/{name}",
        }

    def write_json(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_prepare_repairs_only_urls_and_builds_matching_checksums(self):
        prepare(self.release_path, self.tag_path, self.original, self.repaired, self.contract)
        repaired_manifest = json.loads((self.repaired / MANIFEST_NAME).read_text())
        self.assertEqual(
            repaired_manifest["artifacts"][0]["url"],
            f"https://github.com/Owner/Repo/releases/download/v1.2.3/{self.package_name}",
        )
        original_manifest = json.loads((self.original / MANIFEST_NAME).read_text())
        repaired_without_url = copy.deepcopy(repaired_manifest)
        repaired_without_url["artifacts"][0]["url"] = original_manifest["artifacts"][0]["url"]
        self.assertEqual(repaired_without_url, original_manifest)

        final_release = copy.deepcopy(self.release)
        for asset in final_release["assets"]:
            if asset["name"] in (MANIFEST_NAME, CHECKSUMS_NAME):
                content = (self.repaired / asset["name"]).read_bytes()
                asset["id"] += 10
                asset["size"] = len(content)
                asset["digest"] = f"sha256:{sha256(content)}"
        final_path = self.write_json("final-release.json", final_release)
        verify_final(final_path, self.tag_path, self.repaired, self.repaired / "package-snapshot.json", self.contract)

    def test_prepare_rejects_changed_installer_bytes(self):
        (self.original / self.package_name).write_bytes(b"different")
        with self.assertRaisesRegex(ValueError, "digest changed"):
            prepare(self.release_path, self.tag_path, self.original, self.repaired, self.contract)

    def test_prepare_rejects_a_tag_moved_from_reviewed_commit(self):
        moved = copy.deepcopy(self.tag_ref)
        moved["object"]["sha"] = "b" * 40
        with self.assertRaisesRegex(ValueError, "reviewed commit"):
            prepare(self.release_path, self.write_json("moved.json", moved), self.original, self.repaired, self.contract)

    def test_final_verification_rejects_a_changed_package_url(self):
        prepare(self.release_path, self.tag_path, self.original, self.repaired, self.contract)
        final_release = copy.deepcopy(self.release)
        for asset in final_release["assets"]:
            if asset["name"] in (MANIFEST_NAME, CHECKSUMS_NAME):
                content = (self.repaired / asset["name"]).read_bytes()
                asset["digest"] = f"sha256:{sha256(content)}"
            elif asset["name"] == self.package_name:
                asset["browser_download_url"] += "-changed"
        with self.assertRaisesRegex(ValueError, "installer asset changed"):
            verify_final(
                self.write_json("bad-final.json", final_release),
                self.tag_path,
                self.repaired,
                self.repaired / "package-snapshot.json",
                self.contract,
            )


if __name__ == "__main__":
    unittest.main()
