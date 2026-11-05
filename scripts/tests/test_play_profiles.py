"""Exercise Play profile guards before any upload signing material is supplied."""
from pathlib import Path
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
KEY = (ROOT / "store/google-play/staging-license-public.b64").read_text().strip()


class PlayProfileTests(unittest.TestCase):
    def rejected(self, properties, expected, key=KEY):
        command = ["gradle", ":app:validatePlayRelease", "--offline", "--no-daemon", "--no-configuration-cache",
                   "-Pstore=true", f"-PfileflowLicensePublicKey={key}", *properties]
        env = os.environ.copy()
        for name in list(env):
            if name.startswith("FILEFLOW_UPLOAD_") or name.startswith("ORG_GRADLE_PROJECT_fileflow"):
                env.pop(name)
        result = subprocess.run(command, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=180)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(expected, result.stdout)

    def test_staging_requires_staging_origin_and_suffix(self):
        self.rejected(["-PfileflowStaging=true", "-PfileflowVersionName=0.4.3-staging",
                       "-PfileflowAccountOrigin=https://datalux.anomalousinteractive.com"],
                      "Staging Play bundles require the staging origin")

    def test_valid_staging_profile_reaches_signing_guard(self):
        self.rejected(["-PfileflowStaging=true", "-PfileflowVersionName=0.4.3-staging",
                       "-PfileflowAccountOrigin=https://staging.datalux.anomalousinteractive.com"],
                      "Play releases require all four FILEFLOW_UPLOAD_")
        self.rejected(["-PfileflowStaging=true", "-PfileflowVersionName=0.4.3",
                       "-PfileflowAccountOrigin=https://staging.datalux.anomalousinteractive.com"],
                      "Staging Play bundles require the staging origin")

    def test_production_refuses_staging_origin(self):
        self.rejected(["-PfileflowAccountOrigin=https://staging.datalux.anomalousinteractive.com"],
                      "Production Play bundles require the production origin")

    def test_production_refuses_staging_key(self):
        self.rejected([], "Production Play bundles require the approved production license key")

    def test_staging_rejects_a_different_valid_p256_key(self):
        other = ("MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEXRzuqILx1xFIaDKkuZqxxBUJYhEQ"
                 "QZAZdgnH5ivy6ZUULVr/HZkxCQM03CXOTqmtzAaiiOeQCpURSwq9sKaKuQ==")
        self.rejected(["-PfileflowStaging=true", "-PfileflowVersionName=0.4.3-staging",
                       "-PfileflowAccountOrigin=https://staging.datalux.anomalousinteractive.com"],
                      "Staging Play bundles require the approved staging license key", key=other)

if __name__ == "__main__":
    unittest.main()
