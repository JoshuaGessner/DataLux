import json
import pathlib
import runpy
import tempfile
import unittest
from argparse import Namespace

from package_tools import bundle_digest, sha256_file, write_report


class PackageToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name) / "FileFlow.app"
        binary = self.root / "Contents" / "MacOS" / "FileFlow"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"binary")
        binary.chmod(0o755)
        resources = self.root / "Contents" / "Resources"
        resources.mkdir()
        (resources / "current").symlink_to("asset.dat")
        (resources / "asset.dat").write_bytes(b"asset")

    def tearDown(self):
        self.temp.cleanup()

    def test_bundle_digest_is_stable_and_tracks_bytes_modes_and_symlinks(self):
        original = bundle_digest(self.root)
        self.assertEqual(original, bundle_digest(self.root))
        binary = self.root / "Contents" / "MacOS" / "FileFlow"
        binary.write_bytes(b"changed")
        self.assertNotEqual(original, bundle_digest(self.root))
        changed_bytes = bundle_digest(self.root)
        binary.chmod(0o644)
        self.assertNotEqual(changed_bytes, bundle_digest(self.root))
        binary.chmod(0o755)
        link = self.root / "Contents" / "Resources" / "current"
        link.unlink()
        link.symlink_to("other.dat")
        self.assertNotEqual(changed_bytes, bundle_digest(self.root))

    def test_sha256_file_uses_exact_bytes(self):
        path = pathlib.Path(self.temp.name) / "value"
        path.write_bytes(b"fileflow")
        self.assertEqual(sha256_file(path), "7b7b41c25ae77c3f4dae5675ffa50e7b1131c855ff3fa15738e6a5bb6d84ebcf")

    def test_dmg_layout_preserves_both_historical_and_current_bundle_names(self):
        settings = pathlib.Path(__file__).with_name("dmg_settings.py")
        for name in ("FileFlow.app", "DataLux.app"):
            with self.subTest(name=name):
                app = pathlib.Path(self.temp.name) / name
                config = runpy.run_path(str(settings), init_globals={"defines": {
                    "app": str(app), "metadata": self.temp.name,
                    "background": self.temp.name, "volume_icon": self.temp.name,
                }})
                self.assertIn((str(app), name), config["files"])
                self.assertEqual(config["icon_locations"][name], (170, 220))
                self.assertEqual(config["symlinks"]["Applications"], "/Applications")

    def test_report_binds_package_hash_architecture_and_source(self):
        dmg = pathlib.Path(self.temp.name) / "FileFlow-v1.2.3-macos-arm64.dmg"
        dmg.write_bytes(b"disk image")
        output = pathlib.Path(self.temp.name) / "report.json"
        write_report(Namespace(
            dmg=dmg,
            output=output,
            version="1.2.3",
            arch="arm64",
            source_revision="a" * 40,
            app_tree_sha256="b" * 64,
        ))
        report = json.loads(output.read_text())
        self.assertEqual(report["packageName"], dmg.name)
        self.assertEqual(report["architecture"], "arm64")
        self.assertEqual(report["sourceRevision"], "a" * 40)
        self.assertEqual(report["appTreeSha256"], "b" * 64)
        self.assertEqual(report["sha256"], sha256_file(dmg))


if __name__ == "__main__":
    unittest.main()
