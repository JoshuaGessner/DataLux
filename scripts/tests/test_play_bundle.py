import importlib.util
from pathlib import Path
import struct
import unittest
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("play_bundle", Path(__file__).parents[1] / "check_play_bundle.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def elf(alignment=16_384, relro_end=16_384):
    data = bytearray(256)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, 183)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 2)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, len(data), 16_384, alignment)
    struct.pack_into("<IIQQQQQQ", data, 120, 0x6474E552, 4, 0, 0, 0, 0, relro_end, 1)
    return bytes(data)


class NativeAlignmentTests(unittest.TestCase):
    def test_accepts_16k_library(self):
        MODULE.check_elf(elf())

    def test_rejects_4k_load_segment(self):
        with self.assertRaisesRegex(ValueError, "LOAD segment"):
            MODULE.check_elf(elf(alignment=4096))

    def test_rejects_relro_boundary_that_would_fault_on_16k(self):
        with self.assertRaisesRegex(ValueError, "RELRO"):
            MODULE.check_elf(elf(relro_end=4096))

    def test_accepts_short_relro_when_it_owns_the_entire_load_segment(self):
        data = bytearray(elf(relro_end=4096))
        struct.pack_into("<Q", data, 64 + 40, 4096)  # LOAD MemSiz equals RELRO MemSiz.
        MODULE.check_elf(bytes(data))

    def test_rejects_writable_neighbor_in_the_short_relro_final_page(self):
        data = bytearray(elf(relro_end=4096)) + bytearray(16_384)
        struct.pack_into("<Q", data, 64 + 40, 4096)
        struct.pack_into("<H", data, 56, 3)
        struct.pack_into("<IIQQQQQQ", data, 176, 1, 6, 8192, 8192, 0, 256, 4096, 16_384)
        with self.assertRaisesRegex(ValueError, "RELRO"):
            MODULE.check_elf(bytes(data))

    def test_rejects_truncated_headers(self):
        with self.assertRaisesRegex(ValueError, "headers"):
            MODULE.check_elf(elf()[:128])

    def test_rejects_wrong_architecture(self):
        data = bytearray(elf())
        struct.pack_into("<H", data, 18, 62)
        with self.assertRaisesRegex(ValueError, "arm64"):
            MODULE.check_elf(bytes(data))


class ReleaseManifestTests(unittest.TestCase):
    def manifest(self):
        return ET.fromstring('''<manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="dev.datalux" android:versionCode="5" android:versionName="0.4.1">
            <uses-sdk android:minSdkVersion="33" android:targetSdkVersion="37"/>
            <application android:label="DataLux" android:allowBackup="false"
                android:fullBackupContent="false" android:dataExtractionRules="@xml/data_extraction_rules"
                android:usesCleartextTraffic="false"/>
        </manifest>''')

    def test_accepts_reviewed_release_identity(self):
        result = MODULE.check_manifest(self.manifest(), 5, "0.4.1")
        self.assertEqual(result["appLabel"], "DataLux")
        self.assertFalse(result["appDataBackup"])

    def test_refuses_stale_or_unsafe_release_configuration(self):
        for attribute, value in (("label", "FileFlow"), ("debuggable", "true"),
                                 ("allowBackup", "true"), ("fullBackupContent", "true"),
                                 ("dataExtractionRules", None), ("usesCleartextTraffic", "true")):
            with self.subTest(attribute=attribute):
                manifest = self.manifest()
                app = manifest.find("application")
                if value is None:
                    app.attrib.pop(MODULE.ANDROID + attribute)
                else:
                    app.set(MODULE.ANDROID + attribute, value)
                with self.assertRaises(ValueError):
                    MODULE.check_manifest(manifest, 5, "0.4.1")

    def test_rejects_wrong_package_and_version(self):
        for package in ("dev.fileflow", "another.app"):
            with self.subTest(package=package):
                manifest = self.manifest()
                manifest.set("package", package)
                with self.assertRaisesRegex(ValueError, "application ID"):
                    MODULE.check_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "version code"):
            MODULE.check_manifest(self.manifest(), 6, "0.4.1")

    def test_launcher_shortcuts_target_registered_package_and_existing_classes(self):
        project = Path(__file__).parents[2]
        root = ET.parse(project / "app/src/main/res/xml/shortcuts.xml").getroot()
        intents = root.findall("shortcut/intent")
        self.assertEqual(len(intents), 2)
        for intent in intents:
            self.assertEqual(intent.get(MODULE.ANDROID + "targetPackage"), "dev.datalux")
            self.assertEqual(intent.get(MODULE.ANDROID + "targetClass"), "dev.fileflow.MainActivity")

    def test_source_policy_excludes_all_app_storage_from_both_transports(self):
        project = Path(__file__).parents[2]
        source = ET.parse(project / "app/src/main/AndroidManifest.xml").getroot().find("application")
        self.assertEqual(source.get(MODULE.ANDROID + "allowBackup"), "false")
        self.assertEqual(source.get(MODULE.ANDROID + "fullBackupContent"), "false")
        resource = source.get(MODULE.ANDROID + "dataExtractionRules")
        self.assertTrue(resource.startswith("@xml/"))
        rules = ET.parse(project / f"app/src/main/res/xml/{resource[5:]}.xml").getroot()
        domains = {"root", "file", "database", "sharedpref", "external", "device_root",
                   "device_file", "device_database", "device_sharedpref"}
        for transport in ("cloud-backup", "device-transfer"):
            with self.subTest(transport=transport):
                section = rules.find(transport)
                self.assertIsNotNone(section)
                self.assertFalse(section.findall("include"))
                self.assertEqual({rule.get("domain") for rule in section.findall("exclude")
                                  if rule.get("path") == "."}, domains)


if __name__ == "__main__":
    unittest.main()
