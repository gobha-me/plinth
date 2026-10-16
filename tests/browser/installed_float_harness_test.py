"""Harness construction controls only; never count as the native browser gate."""

import importlib.util
from contextlib import nullcontext
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile


spec = importlib.util.spec_from_file_location(
    'installed_floats', Path(__file__).with_name('run-installed-floats.py'))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
repo = Path(__file__).resolve().parents[2]


class HarnessConstruction(unittest.TestCase):
    def test_twelve_native_groups_preserve_the_original_seven_in_order(self):
        original = [
            'real protected policy, captured profile, authorized/denied surface filtering',
            'ordinary caller SDK openFloat, ready ID, native relative/dynamic imports, shared modules',
            'guarded SQL minimal descriptor and reload new instance without unsaved state',
            'separate user save and fresh authenticated-session restore isolation',
            'required transient field stays live-only and is excluded from storage',
            'actual CSRF, generic write/delete guard, denied pinned admission and module access',
            'normal authenticated package uninstall after consumer contexts close',
        ]
        self.assertEqual(len(harness.EXPECTED_CASES), 12)
        self.assertEqual(len(set(harness.EXPECTED_CASES)), 12)
        indexes = [harness.EXPECTED_CASES.index(name) for name in original]
        self.assertEqual(indexes, sorted(indexes))
        self.assertEqual(harness.EXPECTED_CASES[-1], original[-1])

    def test_fixed_upgrade_variants_preserve_contracts_and_source_without_new_grants(self):
        source = repo / 'tests/extensions/float-provider'
        original = {path.relative_to(source).as_posix(): path.read_bytes()
                    for path in source.rglob('*') if path.is_file()}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging, destination = root / 'source', root / 'zips'
            staging.mkdir()
            destination.mkdir()
            harness.pack_provider_variants(source, staging, destination)
            for version in ['0.2.0', '0.3.0']:
                with self.subTest(version=version), zipfile.ZipFile(destination / f'floatprovider-{version}.zip') as archive:
                    manifest = json.loads(archive.read('manifest.json'))
                    self.assertEqual(manifest['version'], version)
                    manifest['version'] = '0.1.0'
                    self.assertEqual(manifest, json.loads(original['manifest.json']))
                    for name in ['capabilities.json', 'panels.json', 'rbac.json', 'server/main.js',
                                 'server/handlers/preview.js', 'client/panels/preview.js']:
                        self.assertEqual(archive.read(name), original[name])
                    self.assertNotIn('default_grants', json.loads(archive.read('rbac.json')))
                    self.assertEqual(json.loads(archive.read('capabilities.json'))['requires'], [])
                    for info in archive.infolist():
                        self.assertEqual((info.external_attr >> 16) & 0o777,
                                         0o755 if info.is_dir() else 0o644)
                    migration = 'migrations/001_deliberate_failure.sql'
                    if version == '0.2.0':
                        self.assertEqual(archive.read(migration), b'SELECT 1 / 0;\n')
                        self.assertEqual(archive.read('client/panels/label.js'), original['client/panels/label.js'])
                    else:
                        self.assertFalse(any(name.startswith('migrations/') for name in archive.namelist()))
                        self.assertEqual(archive.read('client/panels/label.js'),
                            b"export const label = 'Captured replacement fixture import 0.3.0';\n")
            self.assertEqual({path.relative_to(source).as_posix(): path.read_bytes()
                              for path in source.rglob('*') if path.is_file()}, original)

    def test_fixture_namespaces_handlers_and_panel_paths_match_cross_file_contract(self):
        # Construction parity with CF1/CF3/CF3b/CF4/CF7; this is not execution
        # of the production validator or an API-install success substitute.
        for source in ['float-caller', 'float-provider']:
            with self.subTest(source=source):
                root = repo / 'tests/extensions' / source
                manifest = json.loads((root / 'manifest.json').read_text())
                capabilities = json.loads((root / 'capabilities.json').read_text())
                rules = json.loads((root / 'rbac.json').read_text())
                provided = {item['namespace'] for item in capabilities['provides']}
                self.assertEqual(provided, {manifest['name']})
                self.assertNotIn('default_grants', rules)
                declared = {item['rule'] for item in rules['rules']}
                for rule in rules['rules']:
                    self.assertIn(rule['namespace'], provided, 'CF1 rejects orphan namespaces')
                for capability in capabilities['provides']:
                    self.assertIn(capability['rbac_rule'], declared)
                    self.assertTrue((root / 'server/handlers' / (capability['function'] + '.js')).is_file())
                for panel in json.loads((root / 'panels.json').read_text())['panels']:
                    self.assertIn(panel['rbac_rule'], declared)
                    self.assertTrue((root / 'client/panels' / panel['client_path']).is_file())
                self.assertEqual(capabilities['requires'], [])

    def test_install_failure_summary_is_bounded_and_never_echoes_unknown_data(self):
        script = r'''
import assert from 'node:assert/strict';
import { installFailureSummary } from './installed-float-diagnostics.mjs';
const secret = 'fake-credential-marker-never-diagnostic';
const result = {status: 422, value: {state: 'INSTALL_FAILED', failed_at_stage: 'VALIDATING',
    kind: 'validation-errors', id: secret, message: secret, [secret]: secret}};
const report = {files_scanned: 12, total_bytes: 512, disposition: 'REJECT',
    messages: [{rule: 'rbac-orphan-namespace', message: secret, path: secret}], [secret]: secret};
const encoded = installFailureSummary('floatcaller', result, report), value = JSON.parse(encoded);
assert.equal(value.package, 'floatcaller'); assert.equal(value.httpStatus, 422);
assert.equal(value.code, 'validation-errors'); assert.equal(value.failedAtStage, 'VALIDATING');
assert.deepEqual(value.reportRules, ['rbac-orphan-namespace']);
assert.deepEqual(value.responseKeys, ['state', 'failed_at_stage', 'kind', 'message', 'id']);
assert.deepEqual(value.reportKeys, ['files_scanned', 'total_bytes', 'disposition', 'messages']);
assert(!encoded.includes(secret));
const hostile = JSON.stringify(installFailureSummary(secret, {status: secret,
    value: {error: {code: secret, message: secret}, state: secret, failed_at_stage: secret}},
    {messages: Array(10000).fill({rule: secret, path: secret})}));
assert(!hostile.includes(secret));
const bounded = installFailureSummary('floatprovider', result, {messages: Array(10000).fill(report.messages[0])});
assert(bounded.length <= 2048); assert.equal(JSON.parse(bounded).reportRules.length, 8);
assert.equal(JSON.parse(bounded).reportMessagesCapped, true);
for (const malformed of [null, [], 'not-json']) {
    assert(installFailureSummary('floatcaller', {status: 422, value: malformed}, malformed).length <= 2048);
}
'''
        subprocess.run(['node', '--input-type=module', '-e', script],
                       cwd=repo / 'tests/browser', check=True, timeout=10,
                       capture_output=True, text=True)

    def test_artifact_frame_matches_independently_frozen_binary_vector(self):
        # Expected digest from the merged independent format vector, not from
        # the production encoder or this harness's own output.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / 'hello.txt'
            path.write_bytes(b'A\0B\n')
            path.chmod(0o644)
            self.assertEqual(harness.artifact_digest(root),
                             '2980df2ad79e3c7a334cbea99bc241c784f57bafdee55a8c9fb2aecb4a38411b')

    def test_fixture_zip_has_exact_files_modes_no_default_grants(self):
        for source in ['float-caller', 'float-provider']:
            with self.subTest(source=source), tempfile.TemporaryDirectory() as temporary:
                fixture = repo / 'tests/extensions' / source
                destination = Path(temporary) / 'fixture.zip'
                harness.pack_fixture(fixture, destination)
                with zipfile.ZipFile(destination) as archive:
                    expected = {path.relative_to(fixture).as_posix() + ('/' if path.is_dir() else '')
                                for path in fixture.rglob('*')}
                    self.assertEqual(set(archive.namelist()), expected)
                    for info in archive.infolist():
                        self.assertEqual((info.external_attr >> 16) & 0o777,
                                         0o755 if info.is_dir() else 0o644)
                    self.assertNotIn('default_grants', json.loads(archive.read('rbac.json')))
                    self.assertEqual(json.loads(archive.read('capabilities.json'))['requires'], [])

    def test_reviewable_required_field_and_live_only_control(self):
        declarations = json.loads((repo / 'tests/extensions/float-provider/panels.json').read_text())
        self.assertEqual(declarations['format_version'], 2)
        preview, live = declarations['panels']
        self.assertEqual(preview['context_schema']['persist_keys'], ['record_id'])
        self.assertEqual(preview['context_schema']['fields']['record_id'], {
            'required': True, 'max_bytes': 64, 'meaning': 'Stable fake fixture record identifier'})
        self.assertTrue(live['context_schema']['fields']['live_nonce']['required'])
        self.assertNotIn('live_nonce', live['context_schema']['persist_keys'])

    def test_oversized_stat_refuses_before_opening_or_reading_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (root / 'oversized').open('wb') as file:
                file.truncate(52428801)  # sparse file, no large allocation
            with mock.patch.object(harness.os, 'open', side_effect=AssertionError('must not open')):
                with self.assertRaisesRegex(RuntimeError, 'content profile'):
                    harness.artifact_digest(root)

    def test_streaming_inventory_stops_at_bounded_lookahead(self):
        class Entries:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def __iter__(self):
                for index in range(1000000):
                    seen.append(index)
                    yield SimpleNamespace(path=str(root / str(index)),
                        stat=lambda **_args: SimpleNamespace(st_mode=0o40755))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            seen = []
            with mock.patch.object(harness.os, 'scandir', return_value=Entries()):
                with self.assertRaisesRegex(RuntimeError, 'record profile'):
                    harness.artifact_digest(root)
            self.assertEqual(len(seen), 4097, 'never materialize the unbounded walk')

    def test_cumulative_stat_limit_refuses_second_file_before_open(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'a-small').write_bytes(b'x')
            large = root / 'z-large'
            with large.open('wb') as file:
                file.truncate(52428800)
            with harness.os.scandir(root) as iterator:
                entries = sorted(iterator, key=lambda entry: entry.name)
            actual_open = harness.os.open
            opened = []

            def checked_open(path, flags):
                opened.append(Path(path).name)
                if Path(path) == large:
                    self.fail('cumulative size must refuse before second open')
                return actual_open(path, flags)

            with mock.patch.object(harness.os, 'scandir', return_value=nullcontext(iter(entries))), \
                 mock.patch.object(harness.os, 'open', side_effect=checked_open):
                with self.assertRaisesRegex(RuntimeError, 'content profile'):
                    harness.artifact_digest(root)
            self.assertEqual(opened, ['a-small'])

    def test_private_log_redaction_includes_fake_accounts_even_before_state_exists(self):
        with tempfile.TemporaryDirectory() as temporary:
            state = Path(temporary) / 'state.json'
            env = {'PLINTH_BOOTSTRAP_TOKEN': 'fake-bootstrap', 'PLINTH_PG_PASSWORD': 'fake-pg',
                'PLINTH_FLOAT_PASSWORD': 'fake-before-state', 'PLINTH_FLOAT_STATE': str(state)}
            self.assertEqual(harness.redact_logs('fake-bootstrap fake-pg fake-before-state', env),
                             '[redacted] [redacted] [redacted]')
            state.write_text(json.dumps({'accounts': {'a': {'password': 'fake-account-a'},
                                                     'b': {'password': 'fake-account-b'}}}))
            self.assertEqual(harness.redact_logs('fake-account-a fake-account-b', env),
                             '[redacted] [redacted]')


if __name__ == '__main__':
    unittest.main()
