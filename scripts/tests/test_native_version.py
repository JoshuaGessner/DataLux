import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("native_version", Path(__file__).resolve().parents[1] / "validate_native_version.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NativeVersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "app").mkdir()
        (self.root / "CMakeLists.txt").write_text("project(fileflow LANGUAGES CXX VERSION 0.3.0)\n")
        self.gradle = self.root / "app/build.gradle.kts"
        self.gradle.write_text('val releaseVersionName = providers.gradleProperty("fileflowVersionName").orElse("0.3.0").get()\nversionName = releaseVersionName\n')

    def test_accepts_current_gradle_property_and_prerelease(self):
        MODULE.validate(self.root, "v0.3.0-preview")

    def test_rejects_android_default_mismatch(self):
        self.gradle.write_text(self.gradle.read_text().replace('"0.3.0"', '"0.2.0"'))
        with self.assertRaisesRegex(ValueError, "expected 0.3.0"):
            MODULE.validate(self.root, "0.3.0")

    def test_rejects_native_mismatch(self):
        with self.assertRaisesRegex(ValueError, "expected 0.4.0"):
            MODULE.validate(self.root, "0.4.0")

    def test_rejects_unused_android_default(self):
        self.gradle.write_text(self.gradle.read_text().replace("versionName = releaseVersionName", 'versionName = "0.2.0"'))
        with self.assertRaisesRegex(ValueError, "must use releaseVersionName"):
            MODULE.validate(self.root, "0.3.0")


if __name__ == "__main__":
    unittest.main()
