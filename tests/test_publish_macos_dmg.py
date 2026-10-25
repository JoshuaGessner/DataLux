import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from scripts.publish_macos_dmg import prepare, verify_current
from scripts.repair_release_metadata import Contract, MANIFEST_NAME, CHECKSUMS_NAME

def sha(value):
    return hashlib.sha256(value).hexdigest()

class DmgPublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.original = self.root / 'original'
        self.original.mkdir()
        self.output = self.root / 'metadata'
        self.zip_name = 'FileFlow-v1.2.3-macos-arm64.zip'
        self.dmg = self.root / self.zip_name.replace('.zip', '.dmg')
        self.dmg.write_bytes(b'verified read-only DMG')
        zip_data = b'unchanged signed application ZIP'
        (self.original / self.zip_name).write_bytes(zip_data)
        manifest = {'schemaVersion': 1, 'version': '1.2.3', 'date': '2026-10-25', 'artifacts': [{
            'name': self.zip_name, 'platform': 'macos', 'arch': 'arm64',
            'url': self.url(self.zip_name), 'sha256': sha(zip_data),
            'notes': 'Original ZIP', 'knownIssues': ['Not notarized'],
            'signing': {'status': 'ad-hoc', 'details': 'Ad-hoc bundle'},
        }]}
        manifest_bytes = (json.dumps(manifest) + '\n').encode()
        sums_bytes = f'{sha(zip_data)}  {self.zip_name}\n{sha(manifest_bytes)}  {MANIFEST_NAME}\n'.encode()
        (self.original / MANIFEST_NAME).write_bytes(manifest_bytes)
        (self.original / CHECKSUMS_NAME).write_bytes(sums_bytes)
        self.contract = Contract('Owner/Repo', 42, 'v1.2.3', '1.2.3', 'a' * 40, '2026-10-25', '', {self.zip_name: sha(zip_data)}, sha(manifest_bytes), sha(sums_bytes))
        self.release = {'id':42, 'tag_name':'v1.2.3', 'target_commitish':'a' * 40, 'draft':False, 'prerelease':False, 'immutable':False, 'published_at':'2026-10-25T10:00:00Z', 'assets':[
            self.asset(1, self.zip_name, zip_data), self.asset(2, MANIFEST_NAME, manifest_bytes), self.asset(3, CHECKSUMS_NAME, sums_bytes),
        ]}
        self.tag = {'ref':'refs/tags/v1.2.3', 'object':{'type':'commit', 'sha':'a' * 40}}
        self.report = self.root / 'report.json'
        self.write(self.original / 'release.json', self.release)
        self.write(self.original / 'tag.json', self.tag)
        self.write(self.report, {'schemaVersion':1, 'packageName':self.dmg.name, 'sha256':sha(self.dmg.read_bytes()), 'architecture':'arm64', 'sourceRevision':'a' * 40, 'platform':'macos', 'version':'1.2.3', 'appTreeSha256':'b' * 64})

    def tearDown(self):
        self.temporary.cleanup()

    def url(self, name):
        return 'https://github.com/Owner/Repo/releases/download/v1.2.3/' + name

    def asset(self, identifier, name, data):
        return {'id':identifier, 'name':name, 'size':len(data), 'digest':'sha256:' + sha(data), 'browser_download_url':self.url(name)}

    def write(self, path, value):
        path.write_text(json.dumps(value))

    def plan(self):
        return prepare(self.original, self.dmg, self.report, self.output, self.contract)

    def test_dmg_replaces_only_mac_manifest_row_and_retains_zip_checksum(self):
        self.plan()
        desired = json.loads((self.output / MANIFEST_NAME).read_text())
        self.assertEqual(desired['artifacts'][0]['name'], self.dmg.name)
        self.assertEqual(desired['artifacts'][0]['signing']['status'], 'ad-hoc')
        self.assertEqual(desired['artifacts'][0]['knownIssues'], ['Not notarized'])
        sums = (self.output / CHECKSUMS_NAME).read_text()
        self.assertIn(self.zip_name, sums)
        self.assertIn(self.dmg.name, sums)

    def test_original_and_completed_remote_states_are_safe_to_retry(self):
        original = self.plan()
        verify_current(self.release, self.tag, original, self.dmg, self.output, self.contract)
        completed = copy.deepcopy(self.release)
        completed['assets'].append(self.asset(4, self.dmg.name, self.dmg.read_bytes()))
        for item in completed['assets']:
            if item['name'] in (MANIFEST_NAME, CHECKSUMS_NAME):
                item.update(self.asset(item['id'] + 10, item['name'], (self.output / item['name']).read_bytes()))
        verify_current(completed, self.tag, original, self.dmg, self.output, self.contract)
        # A new run must not silently repackage an already changed source release.
        self.write(self.original / 'release.json', completed)
        with self.assertRaises(ValueError):
            self.plan()

    def test_refuses_changed_app_asset_or_unknown_metadata(self):
        original = self.plan()
        for name in (self.zip_name, MANIFEST_NAME):
            changed = copy.deepcopy(self.release)
            item = next(item for item in changed['assets'] if item['name'] == name)
            item['digest'] = 'sha256:' + 'f' * 64
            with self.subTest(name=name), self.assertRaises(ValueError):
                verify_current(changed, self.tag, original, self.dmg, self.output, self.contract)

    def test_rejects_a_report_for_another_app_source(self):
        report = json.loads(self.report.read_text())
        report['sourceRevision'] = 'f' * 40
        self.write(self.report, report)
        with self.assertRaisesRegex(ValueError, 'verification report'):
            self.plan()

if __name__ == '__main__':
    unittest.main()
