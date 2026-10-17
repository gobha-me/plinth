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
    def test_authority_recovery_requires_ordered_native_frames_request_body_and_empty_primary(self):
        script = r'''
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { observeAuthorityRecovery, authorityRecoveryDOM } from './installed-float-coordination.mjs';
const origin = 'http://127.0.0.1:8099';
const ack = {type: 'subscribed', channels: ['plinth:system:applications.changed'],
    recommended_debounce_ms: 100, recommended_jitter_ms: 20};
const terminal = {type: 'error', error: 'auth_failed', message: 'fake-public-message'};
const frame = (socket, value) => socket.emit('framereceived', {payload: JSON.stringify(value)});
const request = () => ({url: () => origin + '/api/frontend/applications', method: () => 'GET'});
function fixture() {
    const page = new EventEmitter(); let evaluated = 0;
    page.evaluate = async () => { evaluated++; return true; };
    const owner = observeAuthorityRecovery(page, origin), sockets = [];
    const socket = () => {
        const value = new EventEmitter(); value.url = () => 'ws://127.0.0.1:8099/ws/events';
        sockets.push(value); page.emit('websocket', value); return value;
    };
    const old = socket(); frame(old, ack);
    return {page, owner, old, socket, sockets, evaluated: () => evaluated};
}
async function cleanup(f) {
    f.owner.stop(); await f.owner.drain(performance.now() + 1000);
    assert.equal(f.page.eventNames().length, 0);
    assert(f.sockets.every(socket => socket.eventNames().length === 0));
}
const f = fixture(), barrier = f.owner.arm(performance.now() + 1000);
let complete = false; barrier.wait().then(() => { complete = true; }, () => {});
const stale = request(); f.page.emit('request', stale);
frame(f.old, ack); // The old ACK cannot replace the required terminal/new socket.
const beforeTerminal = f.socket(); frame(beforeTerminal, ack);
frame(f.old, terminal);
frame(beforeTerminal, ack); // Socket existed before terminal: still not eligible.
await Promise.resolve(); assert.equal(complete, false);
const next = f.socket();
const beforeAck = request(); f.page.emit('request', beforeAck);
frame(next, ack);
f.page.emit('response', {request: () => beforeAck, status: () => 200,
    body() { throw new Error('pre-ACK request must not be read'); }});
const selected = request(); f.page.emit('request', selected);
let bodyRead = 0, physicalRead = 0, bodyDone, physicalDone;
f.page.emit('response', {request: () => selected, status: () => 200,
    body() { bodyRead++; return new Promise(resolve => { bodyDone = resolve; }); },
    finished() { physicalRead++; return new Promise(resolve => { physicalDone = resolve; }); }});
assert.equal(bodyRead, 1); assert.equal(physicalRead, 1);
bodyDone(Buffer.from('{}')); await Promise.resolve(); assert.equal(f.evaluated(), 0);
physicalDone(null);
assert.deepEqual(await barrier.wait(), {terminal: 1, new_socket_ack: 1, fresh_catalog: 1, home_empty: 1});
assert.equal(f.evaluated(), 1); await cleanup(f);

for (const change of ['old-ack', 'same-socket', 'pre-ack-request', 'missing-body', 'physical-error',
    'http-error', 'wrong-request', 'no-home', 'closed', 'non-string-terminal']) {
    const f = fixture(), barrier = f.owner.arm(performance.now() + 25);
    if (change === 'closed') { f.owner.stop(); }
    else {
        if (change === 'non-string-terminal') frame(f.old, {...terminal, error: ['auth_failed']});
        else if (change !== 'old-ack') frame(f.old, terminal);
        const next = change === 'same-socket' ? f.old : f.socket();
        const selected = request();
        if (change === 'pre-ack-request') f.page.emit('request', selected);
        frame(next, ack);
        if (change !== 'pre-ack-request') f.page.emit('request', selected);
        if (change === 'no-home') f.page.evaluate = async () => false;
        f.page.emit('response', {request: () => change === 'wrong-request' ? request() : selected,
            status: () => change === 'http-error' ? 503 : 200,
            body: () => change === 'missing-body' ? Promise.reject(new Error('fake-body-failure')) : Promise.resolve(Buffer.from('{}')),
            finished: () => Promise.resolve(change === 'physical-error' ? new Error('fake-physical-failure') : null)});
    }
    await assert.rejects(barrier.wait()); await cleanup(f);
}
const noAckPage = new EventEmitter(), noAck = observeAuthorityRecovery(noAckPage, origin);
assert.throws(() => noAck.arm(performance.now() + 100)); noAck.stop();
const expired = fixture(); assert.throws(() => expired.owner.arm(performance.now() - 1)); await cleanup(expired);

// Stop rejects the logical checkpoint but late selected body work stays owned
// until the explicit post-browser-close drain; it never evaluates old DOM.
const late = fixture(), lateBarrier = late.owner.arm(performance.now() + 1000);
frame(late.old, terminal); const lateSocket = late.socket(); frame(lateSocket, ack);
const lateRequest = request(); late.page.emit('request', lateRequest); let release;
late.page.emit('response', {request: () => lateRequest, status: () => 200,
    body: () => new Promise(resolve => { release = resolve; }), finished: () => Promise.resolve(null)});
late.owner.stop(); await assert.rejects(lateBarrier.wait());
let drained = false; const joined = late.owner.drain(performance.now() + 1000).then(() => { drained = true; });
await Promise.resolve(); assert.equal(drained, false); release(Buffer.from('{}')); await joined;
assert.equal(late.evaluated(), 0); await cleanup(late);

const broken = fixture(), brokenBarrier = broken.owner.arm(performance.now() + 1000);
frame(broken.old, terminal); const brokenSocket = broken.socket(); frame(brokenSocket, ack);
const brokenRequest = request(); broken.page.emit('request', brokenRequest);
const exactBodyError = new Error('fake exact body failure'); let releasePhysical;
broken.page.emit('response', {request: () => brokenRequest, status: () => 200,
    body: () => Promise.reject(exactBodyError),
    finished: () => new Promise(resolve => { releasePhysical = resolve; })});
try { await brokenBarrier.wait(); assert.fail('body failure cannot pass'); }
catch (error) { assert.equal(error, exactBodyError, 'preserve the actual first rejection'); }
broken.owner.stop(); let physicalJoined = false;
const physicalJoin = broken.owner.drain(performance.now() + 1000).then(() => { physicalJoined = true; });
await Promise.resolve(); assert.equal(physicalJoined, false, 'rejected body does not release pending physical work');
releasePhysical(null); await physicalJoin; assert.equal(broken.evaluated(), 0); await cleanup(broken);

// Actual event-driven DOM helper: disconnects its MutationObserver and clears
// its timer on both success and deadline, without polling or DOM mutation.
let observeMutation, disconnected = 0, ready = false, tilePresent = true, tileInside = true;
globalThis.MutationObserver = class { constructor(callback) { observeMutation = callback; }
    observe() {} disconnect() { disconnected++; } };
globalThis.document = {documentElement: {}, querySelector: selector =>
    selector === '.launcher-main' ? {getAttribute: () => ready ? 'false' : 'true'} :
    selector === '.launcher-home' ? {hidden: !ready, contains: () => tileInside} :
    selector === '.panel-host' ? {isConnected: true, childElementCount: ready ? 0 : 1} :
    selector.includes('.launcher-tile') && tilePresent ? {isConnected: true} : null};
const dom = authorityRecoveryDOM(1000); ready = true; observeMutation(); assert.equal(await dom, true);
assert.equal(disconnected, 1); ready = false;
await assert.rejects(authorityRecoveryDOM(5)); assert.equal(disconnected, 2);
ready = true; tilePresent = false;
await assert.rejects(authorityRecoveryDOM(5)); assert.equal(disconnected, 3, 'failed empty Home has no authorized caller tile');
tilePresent = true; tileInside = false;
await assert.rejects(authorityRecoveryDOM(5)); assert.equal(disconnected, 4, 'tile must belong to the current Home');
'''
        subprocess.run(['node', '--input-type=module', '-e', script],
                       cwd=repo / 'tests/browser', check=True, timeout=10,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_private_caller_timeout_observation_is_bounded_and_contains_no_free_text(self):
        script = r'''
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { EventEmitter } from 'node:events';
const source = readFileSync('installed-float-production.mjs', 'utf8');
const start = source.indexOf('function callerTimeoutObservation() {');
const end = source.indexOf('\nasync function captureFailurePages(', start);
assert(start > 0 && end > start);
const observe = new Function('return (' + source.slice(start, end) + ')')();
const secret = 'fake-credential-url-record-id-never-retained';
let output = { textContent: secret }, caller = {getAttribute: () => '1'};
const host = {isConnected: true, hidden: false, childElementCount: 1000};
globalThis.document = {visibilityState: 'hidden', querySelector: selector =>
    selector === '#installed-float-caller' ? caller : selector === '#installed-float-open-result' ? output :
        selector === '.panel-host' ? host : {hidden: true}, querySelectorAll: () => ({length: 1000})};
const entry = {incarnation: 1, mounted: true, mounts: 1, unmounts: 0, open_starts: 1,
    resolved: 0, rejected: 1, suppressed: 0, last_code: 'missing-handler', ignored: secret};
globalThis.__plinthInstalledFloats = {callers: {instances: [entry], events: 3, overflow: false, lastStarted: 1}};
let value = observe();
assert.equal(value.visibility, 'hidden'); assert.equal(value.output, 'other');
assert.equal(value.current.last_code, 'missing-handler'); assert.equal(value.current_is_last_open, true);
assert.equal(value.primary_host_connected, true); assert.equal(value.primary_child_count, 64);
assert.equal(value.provider_count, 64); assert.equal(value.home_hidden, true);
assert(!JSON.stringify(value).includes(secret)); assert(!JSON.stringify(value).includes('incarnation'));
for (const [text, expected] of [['idle', 'idle'], ['opening', 'opening'], ['opened:' + secret, 'opened'],
    ['refused:' + secret, 'refused'], ['refused:' + 'x'.repeat(129), 'other']]) {
    output.textContent = text; assert.equal(observe().output, expected);
}
entry.last_code = secret; entry.mounts = 1000; entry.rejected = NaN;
document.visibilityState = secret;
__plinthInstalledFloats.callers.instances = Array.from({length: 100}, () => entry);
__plinthInstalledFloats.callers.events = 1000; __plinthInstalledFloats.callers.overflow = true;
value = observe(); assert.equal(value.instance_count, 64); assert.equal(value.event_count, 0);
assert.equal(value.totals.open_starts, 64); assert.equal(value.current.mounts, 0);
assert.equal(value.current.last_code, 'unknown'); assert.equal(value.visibility, 'unknown');
assert(!JSON.stringify(value).includes(secret));
caller = null; output = null;
value = observe(); assert.equal(value.caller_present, false); assert.equal(value.output, 'absent');
assert.equal(value.current, null); assert.equal(value.current_is_last_open, null);
assert(source.includes("console.error('PLINTH_FLOAT_PRIVATE_UI ' + JSON.stringify(await captureFailurePages()))"));
assert(source.includes("performance.now() + 2000"));
assert(source.includes('Preserve the exact original failure even if diagnostics fail.'));
const refusal = source.slice(source.indexOf('async function refusedOpen('), source.indexOf('\nasync function revalidateFromUI('));
assert.deepEqual([...refusal.matchAll(/stage = '([^']+)'/g)].map(match => match[1]),
    ['initial-caller', 'fill', 'barrier', 'click', 'native-body', 'refusal-wait', 'refusal-value']);
assert(refusal.indexOf('try {') < refusal.indexOf('await openCaller('));
assert(refusal.includes('throw error;\n    } finally { observer?.stop(); }'));
assert.equal((refusal.match(/\.click\(/g) ?? []).length, 1);
assert.equal((refusal.match(/page\.waitForFunction\(/g) ?? []).length, 1);
assert(!refusal.includes('PLINTH_FLOAT_PRIVATE_UI'));
assert.equal((source.match(/console\.error\('PLINTH_FLOAT_PRIVATE_UI /g) ?? []).length, 1);
const snapshotStart = source.indexOf('async function captureFailurePages() {');
const snapshotEnd = source.indexOf('\nasync function refusedOpen(', snapshotStart);
const captureSource = source.slice(snapshotStart, snapshotEnd);
const snapshots = new Set(); let selected = 0;
const contexts = Array.from({length: 10}, () => ({pages: () => Array.from({length: 10}, () => ({
    evaluate: async () => { selected++; return {schema_version: 1, output: 'refused'}; } }))}));
let sharedDeadline, boundedCalls = 0;
const bounded = async (run, deadline) => { boundedCalls++; sharedDeadline = deadline; return await run(); };
const capture = new Function('contexts', 'privateSnapshotTasks', 'boundedStep', 'callerTimeoutObservation',
    'refusedFailureStage', 'privateRealtime', 'return (' + captureSource + ')')(contexts, snapshots, bounded, observe, 'click', new Map());
const globalValue = await capture();
assert.equal(selected, 8); assert.equal(snapshots.size, 8); assert.equal(boundedCalls, 1);
assert(Number.isFinite(sharedDeadline)); assert.equal(globalValue.page_count, 8);
assert.equal(globalValue.context_count, 8); assert.equal(globalValue.overflow, true);
assert.equal(globalValue.refusal_stage, 'click');
assert.deepEqual(globalValue.pages.map(page => page.page_ordinal), [1,2,3,4,5,6,7,8]);
assert(globalValue.pages.every(page => page.context_ordinal === 1 && page.state === 'observed'));
assert(!JSON.stringify(globalValue).includes(secret));
await Promise.all([...snapshots]);
const pendingTasks = new Set(); let release;
const pendingContexts = [{pages: () => [{evaluate: () => new Promise(resolve => { release = resolve; })},
    {evaluate: () => Promise.reject(new Error(secret))}]}, {pages() { throw new Error(secret); }}];
const pendingCapture = new Function('contexts', 'privateSnapshotTasks', 'boundedStep', 'callerTimeoutObservation',
    'refusedFailureStage', 'privateRealtime', 'return (' + captureSource + ')')(pendingContexts, pendingTasks,
        async () => { await Promise.resolve(); throw new Error('bounded diagnostic deadline'); }, observe, secret, new Map());
const pendingValue = await pendingCapture();
assert.equal(pendingValue.refusal_stage, null); assert.deepEqual(pendingValue.unavailable_context_ordinals, [2]);
assert.equal(pendingValue.pages[0].state, 'pending'); assert.equal(pendingValue.pages[1].state, 'unavailable');
assert(!JSON.stringify(pendingValue).includes(secret));
release({schema_version: 1, output: 'idle'}); await Promise.all([...pendingTasks]);
assert.equal(pendingValue.pages[0].state, 'observed', 'root retains and joins late diagnostic settlement');
assert(source.includes('[...bodyObservationTasks, ...privateSnapshotTasks]'));
assert(source.indexOf('await captureFailurePages()') < source.indexOf('for (const lane of readLanes.values()) lane.stop();'));
const wsStart = source.indexOf('function observePrivateRealtime(page) {');
const wsEnd = source.indexOf('\nasync function login(', wsStart);
const pageMap = new Map();
const observeWs = new Function('privateRealtime', 'privateRealtimeClosed',
    'return (' + source.slice(wsStart, wsEnd) + ')')(pageMap, false);
const wsPage = new EventEmitter(); observeWs(wsPage);
const socket = new EventEmitter(); wsPage.emit('websocket', socket);
const emit = value => socket.emit('framereceived', {payload: JSON.stringify(value)});
for (let index = 0; index < 70; index++) emit({type: 'error', error: 'auth_failed', message: secret});
emit({type: 'subscribed', channels: ['plinth:system:applications.changed'],
    recommended_debounce_ms: 100, recommended_jitter_ms: 20});
for (const payload of [secret, 'x'.repeat(2049), 'é'.repeat(1025), new Uint8Array([1]), '{"type":NaN}'])
    socket.emit('framereceived', {payload});
emit({type: 'error', error: secret, message: secret});
emit({type: 'error', error: 'auth_failed', message: secret, extra: secret});
for (const error of [['auth_timeout'], {value: 'auth_timeout'}, null, true, false])
    emit({type: 'error', error, message: secret});
emit({type: 'subscribed', channels: [secret], recommended_debounce_ms: 100, recommended_jitter_ms: 20});
const wsSnapshot = pageMap.get(wsPage).snapshot();
assert.equal(wsSnapshot.terminals.auth_failed, 64); assert.equal(wsSnapshot.application_acks, 1);
assert.equal(wsSnapshot.terminals.auth_timeout, 0, 'non-string error must not coerce into a known terminal');
assert.equal(wsSnapshot.frames, 64); assert(!JSON.stringify(wsSnapshot).includes(secret));
const extraSockets = Array.from({length: 20}, () => new EventEmitter());
for (const value of extraSockets) wsPage.emit('websocket', value);
assert.equal(pageMap.get(wsPage).snapshot().sockets, 16); assert.equal(pageMap.get(wsPage).snapshot().overflow, true);
for (let index = 0; index < 20; index++) observeWs(new EventEmitter());
assert.equal(pageMap.size, 8);
pageMap.get(wsPage).stop(); assert.equal(wsPage.eventNames().length, 0);
assert.equal(socket.eventNames().length, 0); assert(extraSockets.every(value => value.eventNames().length === 0));
const closedMap = new Map(), latePage = new EventEmitter();
const observeClosed = new Function('privateRealtime', 'privateRealtimeClosed',
    'return (' + source.slice(wsStart, wsEnd) + ')')(closedMap, true);
observeClosed(latePage); assert.equal(closedMap.size, 0); assert.equal(latePage.eventNames().length, 0);
assert(source.indexOf('observePrivateRealtime(page);') < source.indexOf("await page.goto(base + '/app/')"));
assert(source.indexOf('for (const observer of privateRealtime.values()) observer.stop();') <
    source.indexOf('for (const context of contexts) { try { await context.close();'));
assert(source.indexOf('privateRealtimeClosed = true;') < source.indexOf('for (const observer of privateRealtime.values()) observer.stop();'));
const fixture = readFileSync('../extensions/float-caller/client/panels/caller.js', 'utf8');
assert(fixture.includes('hooks.useEffect(() => {'));
assert(!fixture.includes('useLayoutEffect'));
assert(fixture.includes('callers.instances.length < 64'));
assert(fixture.includes('Math.min(64, entry[key] + 1)'));
assert(fixture.includes('Math.min(64, callers.events + 1)'));
assert(fixture.includes("if (!mounted.current) count(observation.current, 'suppressed')"));
assert(fixture.includes("if (mounted.current) setStatus('refused:' +"));
'''
        subprocess.run(['node', '--input-type=module', '-e', script],
                       cwd=repo / 'tests/browser', check=True, timeout=10,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

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
const transition = installFailureSummary('floatprovider', {status: 409,
    value: {error: {code: 'in-flight-operation', message: secret}}}, null);
assert.equal(JSON.parse(transition).code, 'in-flight-operation');
assert.equal(JSON.parse(transition).httpStatus, 409);
assert(!transition.includes(secret));
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

    def test_journey_failure_summary_only_emits_finite_identifiers(self):
        script = r'''
import assert from 'node:assert/strict';
import { journeyFailurePrefix, journeyFailureSummary, emitJourneyFailure, createJourneyFailureSteps,
    observeSelectedBody, bodyErrorKind } from './installed-float-diagnostics.mjs';
import { EventEmitter } from 'node:events';
const secret = 'fake-password-query-id-never-public';
const error = {name: 'AssertionError', code: 'ERR_ASSERTION', operator: 'deepStrictEqual',
    message: secret, actual: secret, expected: secret,
    stack: `${secret}\n    at open (file:///private/${secret}/installed-float-production.mjs:127:9)\n`};
const raw = journeyFailureSummary('verify', ['group one'], error), value = JSON.parse(raw);
assert.equal(journeyFailurePrefix, 'PLINTH_FLOAT_FAILURE ');
assert(raw.length <= 2048); assert(!raw.includes(secret));
assert.deepEqual(value, {schema_version: 1, phase: 'verify', completed_count: 1, completed_groups: [1],
    source: {file: 'installed-float-production.mjs', line: 127, column: 9},
    failure_step: null, body_observation: null, category: 'assertion', code: 'ERR_ASSERTION', operator: 'deepStrictEqual'});
for (const frame of [`    at open (/private/${secret}/installed-float-production.mjs:127:9)`,
    '    at async tests/browser/installed-float-production.mjs:127:9',
    '    at installed-float-production.mjs:127:9',
    `    at Timeout.fail [as _onTimeout] (file:///private/${secret}/installed-float-production.mjs:127:9)`,
    `    at Timeout.fail [as _onTimeout] (/private/${secret}/installed-float-production.mjs:127:9)`]) {
    const summary = journeyFailureSummary('verify', ['one'], {...error, stack: secret + '\n' + frame}, 'personal-open');
    assert(!summary.includes(secret));
    assert.deepEqual(JSON.parse(summary).source, value.source);
    assert.equal(JSON.parse(summary).failure_step, 'personal-open');
}
for (const stack of [secret, `${secret}:123:4`, `    at file:///private/${secret}.mjs:1:2`,
    `    at Timeout.fail [as ${secret}] (file:///private/installed-float-production.mjs:127:9)`,
    `    at ${'x'.repeat(129)} [as _onTimeout] (file:///private/installed-float-production.mjs:127:9)`,
    '    at Timeout.fail [as _onTimeout] (file:///private/installed-float-production.mjs:1000000:9)',
    '    at Timeout.fail [as _onTimeout] (file:///private/installed-float-production.mjs:127:0)',
    '    at Timeout.fail [as _onTimeout] (file:///private/another.mjs:127:9)',
    'x'.repeat(20000) + error.stack]) {
    const encoded = journeyFailureSummary('verify', [], {name: secret, code: secret, operator: secret,
        message: secret, actual: secret, stack});
    assert(!encoded.includes(secret));
    assert.deepEqual(JSON.parse(encoded).source, {file: 'unknown', line: 0, column: 0});
    assert.equal(JSON.parse(encoded).category, 'unknown');
}
assert.equal(JSON.parse(journeyFailureSummary('verify', [], {name: 'TimeoutError'})).category, 'timeout');
assert.equal(JSON.parse(journeyFailureSummary('install', [], new Error(secret))).completed_count, 0);
for (const property of ['stack', 'name', 'code', 'operator']) {
    const hostile = new Error(secret), output = [];
    Object.defineProperty(hostile, property, {get() { throw new Error(secret); }});
    assert.doesNotThrow(() => emitJourneyFailure('verify', [], hostile, line => output.push(line)));
    assert.deepEqual(output, [journeyFailurePrefix + '{"diagnostic":"unavailable"}']);
    assert.doesNotThrow(() => emitJourneyFailure('verify', [], hostile, () => { throw new Error(secret); }));
}
const output = [];
emitJourneyFailure('verify', ['group one'], error, line => output.push(line));
assert.deepEqual(output, [journeyFailurePrefix + raw]);
const labels = ['initial-caller', 'default-state', 'personal-write', 'personal-open', 'personal-close',
    'personal-delete', 'admin-seed', 'admin-open', 'admin-close', 'admin-delete',
    'fallback-open', 'fallback-focus', 'fallback-modules'];
for (const label of labels) {
    assert.equal(JSON.parse(journeyFailureSummary('verify', ['one'], error, label)).failure_step, label);
}
assert.equal(JSON.parse(journeyFailureSummary('verify', ['one'], error, secret)).failure_step, null);
assert.equal(JSON.parse(journeyFailureSummary('verify', ['one', 'two'], error, 'personal-open')).failure_step, null);
assert.equal(JSON.parse(journeyFailureSummary('install', [], error, 'personal-open')).failure_step, null);
for (const rejected of [new Error(secret), null, undefined, false, 0, '']) {
    const steps = createJourneyFailureSteps();
    assert.equal(steps.run('personal-write', () => 200), 200);
    assert.equal(await steps.run('personal-write', async () => 200), 200);
    assert.equal(steps.failed(), null, 'successful substeps cannot leave a stale label');
    let caught = false;
    try { await steps.run('personal-open', () => Promise.reject(rejected)); }
    catch (actual) { caught = true; assert.equal(actual, rejected); }
    assert(caught); assert.equal(steps.failed(), 'personal-open');
    steps.run('personal-delete', () => 200);
    assert.equal(steps.failed(), 'personal-open', 'successful cleanup preserves the actual failed step');
    const cleanup = new Error(secret);
    try { steps.run('personal-delete', () => { throw cleanup; }); }
    catch (actual) { assert.equal(actual, cleanup); }
    assert.equal(steps.failed(), 'personal-delete', 'later finally failure matches the final caught error');
}
const advice = '\nResponse body is not available for a response that was navigated away from. Read response.body() before triggering any navigation.';
const knownBodies = [
    ['response.body: Protocol error (Network.getResponseBody): No data found for resource with given identifier', 'cdp-no-data'],
    ['response.body: Protocol error (Network.getResponseBody): No resource with given identifier found', 'cdp-no-resource'],
    ['response.body: Protocol error (Network.getResponseBody): Request content was evicted from inspector cache', 'cdp-evicted'],
    ['response.body: Response body is unavailable', 'pw-detached'],
    ['response.body: Target page, context or browser has been closed', 'page-closed']];
for (const [message, kind] of knownBodies) {
    assert.equal(bodyErrorKind(new Error(message)), kind);
    assert.equal(bodyErrorKind(new Error(message + advice)), kind);
    assert.equal(bodyErrorKind(new Error(message + secret)), 'unknown');
}
assert.equal(bodyErrorKind({get message() { throw new Error(secret); }}), 'unknown');
assert.equal(bodyErrorKind(new Error('x'.repeat(4097))), 'unknown');
const page = new EventEmitter(); let detached = false, finish;
const frame = {isDetached: () => detached}; page.mainFrame = () => frame;
const nativeRequest = {frame: () => frame, resourceType: () => 'fetch', failure: () => ({errorText: 'net::ERR_ABORTED'})};
const response = {request: () => nativeRequest, status: () => 200, finished: () => new Promise(resolve => { finish = resolve; })};
const observation = observeSelectedBody(page, response);
page.emit('requestfinished', {}); // Never correlate another native request.
for (let index = 0; index < 20; index++) { page.emit('framenavigated', frame); page.emit('framedetached', {}); }
page.emit('requestfailed', nativeRequest); detached = true;
const original = new Error(knownBodies[0][0] + advice), properties = Object.getOwnPropertyNames(original);
observation.reject(original); finish(null); await observation.finished; observation.stop();
assert.deepEqual(Object.getOwnPropertyNames(original), properties, 'diagnostics cannot mutate the original Error');
const observed = JSON.parse(journeyFailureSummary('verify', ['one'], original, 'personal-open'));
assert.deepEqual(observed.body_observation, {body_error: 'cdp-no-data', status: 200, resource_type: 'fetch',
    frame: 'detached', request_state: 'failed', request_failure: 'aborted', response_finished: 'ok',
    main_frame_navigations: 16, frame_detachments: 16});
assert.equal(JSON.parse(journeyFailureSummary('verify', ['one'], new Error(original.message))).body_observation, null,
    'same text is not the same selected body Error');
assert.equal(page.eventNames().length, 0, 'all passive event listeners must stop');
assert(!JSON.stringify(observed).includes('Network.getResponseBody'));
const noFrameRequest = {frame() { throw new Error(secret); }, resourceType: () => secret};
const second = observeSelectedBody(page, {request: () => noFrameRequest, status: () => 503,
    finished: () => Promise.reject(new Error(secret))});
const unknown = new Error(secret); second.reject(unknown); await second.finished; second.stop();
const secondValue = JSON.parse(journeyFailureSummary('verify', [], unknown)).body_observation;
assert.equal(secondValue.resource_type, 'other'); assert.equal(secondValue.frame, 'unavailable');
assert.equal(secondValue.response_finished, 'error'); assert.equal(secondValue.request_state, 'unobserved');
assert.equal(secondValue.body_error, 'unknown'); assert(!JSON.stringify(secondValue).includes(secret));
'''
        subprocess.run(['node', '--input-type=module', '-e', script],
                       cwd=repo / 'tests/browser', check=True, timeout=10,
                       capture_output=True, text=True)

        # Exercise Node's actual named-timer stack rather than only spelling a
        # synthetic alias. Only this known basename and finite coordinates emit.
        with tempfile.TemporaryDirectory(prefix='plinth-timer-diagnostic-') as temporary:
            fixture = Path(temporary) / 'installed-float-production.mjs'
            module = (repo / 'tests/browser/installed-float-diagnostics.mjs').as_uri()
            fixture.write_text("import assert from 'node:assert/strict';\n" +
                'import { journeyFailureSummary } from ' + json.dumps(module) + ';\n' + r'''
const secret = 'fake-timer-password-never-public';
const observed = await new Promise(resolve => {
    function fail() { resolve(new Error(secret)); }
    setTimeout(fail, 0);
});
assert(observed.stack.includes('Timeout.fail [as _onTimeout]'));
const encoded = journeyFailureSummary('verify', ['one'], observed, 'personal-open');
const value = JSON.parse(encoded);
assert.equal(value.source.file, 'installed-float-production.mjs');
assert(Number.isInteger(value.source.line) && value.source.line > 0 && value.source.line <= 999999);
assert(Number.isInteger(value.source.column) && value.source.column > 0 && value.source.column <= 999999);
assert.equal(value.category, 'operation');
assert.equal(value.failure_step, 'personal-open');
assert(!encoded.includes(secret));
assert(!encoded.includes('Timeout.fail'));
assert(!encoded.includes('file:'));
''', encoding='utf-8')
            subprocess.run(['node', str(fixture)], check=True, timeout=10,
                           capture_output=True, text=True)

    def test_supervisor_rejects_unsafe_or_malformed_failure_summaries(self):
        value = {'schema_version': 1, 'phase': 'verify', 'completed_count': 1,
            'completed_groups': [1], 'source': {'file': 'installed-float-production.mjs', 'line': 127, 'column': 9},
            'failure_step': None, 'body_observation': None, 'category': 'assertion', 'code': 'ERR_ASSERTION', 'operator': 'deepStrictEqual'}
        prefix = harness.FAILURE_PREFIX
        encoded = prefix + json.dumps(value)
        secret = 'fake-credential-marker-never-public'
        self.assertEqual(harness.safe_browser_failure(secret + '\n' + encoded, 'verify'), value)
        for invalid in [None, secret, encoded[:-2], encoded + '\n' + encoded,
                        prefix + '{"schema_version":1,"schema_version":1}',
                        prefix + '[]', prefix + '{"value":' + '[' * 1500,
                        prefix + 'x' * 2049, 'x' * 65537, '\ud800']:
            with self.subTest(kind=type(invalid).__name__):
                self.assertEqual(harness.safe_browser_failure(invalid, 'verify'), {'diagnostic': 'unavailable'})
        for key, bad in [('phase', secret), ('completed_count', True), ('completed_count', 13),
                         ('completed_groups', [True]), ('completed_groups', [2]),
                         ('source', {'file': secret, 'line': 1, 'column': 2}),
                         ('source', {'file': 'unknown', 'line': 1, 'column': 0}),
                         ('source', {'file': 'installed-float-production.mjs', 'line': 1000000, 'column': 1}),
                         ('category', secret), ('code', secret), ('operator', {}), ('extra', secret),
                         ('failure_step', secret), ('failure_step', []), ('failure_step', False),
                         ('failure_step', 'x' * 2049)]:
            changed = dict(value, **{key: bad})
            result = harness.safe_browser_failure(prefix + json.dumps(changed), 'verify')
            self.assertEqual(result, {'diagnostic': 'unavailable'})
            self.assertNotIn(secret, json.dumps(result))
        self.assertEqual(harness.safe_browser_failure(encoded, 'install'), {'diagnostic': 'unavailable'})
        self.assertEqual(harness.safe_browser_failure(encoded.replace('127', 'NaN'), 'verify'),
                         {'diagnostic': 'unavailable'})
        for constant in ['NaN', 'Infinity', '-Infinity']:
            invalid = encoded.replace('"failure_step": null', '"failure_step": ' + constant)
            self.assertNotEqual(invalid, encoded)
            self.assertEqual(harness.safe_browser_failure(invalid, 'verify'), {'diagnostic': 'unavailable'})
        self.assertEqual(len(harness.FAILURE_STEPS), 13)
        for step in harness.FAILURE_STEPS:
            changed = dict(value, failure_step=step)
            self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(changed), 'verify'), changed)
            changed.update(completed_count=2, completed_groups=[1, 2])
            self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(changed), 'verify'),
                             {'diagnostic': 'unavailable'})
        missing = dict(value)
        missing.pop('failure_step')
        self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(missing), 'verify'),
                         {'diagnostic': 'unavailable'})
        body = {'body_error': 'cdp-no-data', 'status': 200, 'resource_type': 'fetch', 'frame': 'attached',
            'request_state': 'finished', 'request_failure': 'none', 'response_finished': 'ok',
            'main_frame_navigations': 0, 'frame_detachments': 0}
        with_body = dict(value, body_observation=body)
        self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(with_body), 'verify'), with_body)
        for key, bad in [('body_error', secret), ('status', True), ('status', 600),
                         ('resource_type', secret), ('frame', secret), ('request_state', []),
                         ('request_failure', 'aborted'), ('response_finished', {}),
                         ('main_frame_navigations', True), ('frame_detachments', 17), ('extra', secret)]:
            invalid = dict(value, body_observation=dict(body, **{key: bad}))
            result = harness.safe_browser_failure(prefix + json.dumps(invalid), 'verify')
            self.assertEqual(result, {'diagnostic': 'unavailable'})
            self.assertNotIn(secret, json.dumps(result))
        for bad in [False, [], secret, {'body_error': 'unknown'}]:
            self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(dict(value, body_observation=bad)), 'verify'),
                             {'diagnostic': 'unavailable'})
        missing = dict(value)
        missing.pop('body_observation')
        self.assertEqual(harness.safe_browser_failure(prefix + json.dumps(missing), 'verify'),
                         {'diagnostic': 'unavailable'})

    def test_passive_read_lane_requires_bodies_and_coalesced_ui_settlement(self):
        script = r'''
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { observeReadLane } from './installed-float-coordination.mjs';
const origin = 'http://127.0.0.1:4321';
const deferred = () => { let resolve; const promise = new Promise(yes => { resolve = yes; }); return {promise, resolve}; };
const request = path => ({url: () => origin + path, method: () => 'GET'});
const page = new EventEmitter(), barriers = [];
page.evaluate = () => { const gate = deferred(); barriers.push(gate); return gate.promise; };
const lane = observeReadLane(page, origin), first = request('/api/frontend/applications'), body = deferred();
page.emit('request', first);
page.emit('response', {request: () => first, status: () => 200, finished: () => body.promise});
let done = false;
const drained = lane.drain(performance.now() + 1000).then(() => { done = true; });
await Promise.resolve(); assert(!done); assert.equal(barriers.length, 0); assert.equal(lane.catalogCompleted(), 0);
body.resolve(null);
for (let index = 0; index < 8; index++) await Promise.resolve();
assert.equal(lane.catalogCompleted(), 1); assert.equal(barriers.length, 1); assert(!done);
const coalesced = request('/api/frontend/float-layout'), nextBody = deferred();
page.emit('request', coalesced);
page.emit('response', {request: () => coalesced, status: () => 200, finished: () => nextBody.promise});
barriers[0].resolve();
for (let index = 0; index < 8; index++) await Promise.resolve();
assert(!done); assert.equal(barriers.length, 1);
nextBody.resolve(null);
for (let index = 0; index < 8; index++) await Promise.resolve();
assert.equal(barriers.length, 2); barriers[1].resolve(); await drained; assert(done);
lane.stop(); lane.stop();
for (const event of ['request', 'response', 'requestfailed']) assert.equal(page.listenerCount(event), 0);
'''
        subprocess.run(['node', '--input-type=module', '-e', script], cwd=repo / 'tests/browser',
                       check=True, timeout=10, capture_output=True, text=True)

    def test_passive_read_lane_is_bounded_and_has_no_early_idle_success(self):
        script = r'''
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { boundedStep, createResponseBodyRegistry, observeReadLane } from './installed-float-coordination.mjs';
const origin = 'http://127.0.0.1:4321';
const makePage = () => Object.assign(new EventEmitter(), {evaluate: async () => {}});
const request = () => ({url: () => origin + '/api/frontend/surfaces/admission', method: () => 'POST'});
let calls = 0;
await assert.rejects(boundedStep(() => { calls++; }, performance.now() - 1)); assert.equal(calls, 0);
const page = makePage(), lane = observeReadLane(page, origin);
for (let index = 0; index < 65; index++) page.emit('request', request());
await assert.rejects(lane.drain(performance.now() + 100)); lane.stop();
const waitingPage = makePage(), waitingLane = observeReadLane(waitingPage, origin);
waitingPage.emit('request', request());
await assert.rejects(waitingLane.drain(performance.now() + 5)); waitingLane.stop();
const idlePage = makePage(), idleLane = observeReadLane(idlePage, origin);
let barriers = 0; idlePage.evaluate = async () => { barriers++; };
await idleLane.drain(performance.now() + 100); assert.equal(barriers, 1); idleLane.stop();
const failedPage = makePage(), failedLane = observeReadLane(failedPage, origin), failed = request();
failedPage.emit('request', failed); failedPage.emit('requestfailed', failed);
await assert.rejects(failedLane.drain(performance.now() + 100)); failedLane.stop();

// A native Response is lazy: capture must start before its resource disappears.
let valid = true, bodyCalls = 0;
const owned = createResponseBodyRegistry();
const captured = owned.capture({body() {
    bodyCalls++; assert(valid, 'late body lookup after navigation');
    return Promise.resolve(Buffer.from('{"owned":1}'));
}});
assert.equal(bodyCalls, 1); valid = false;
assert.deepEqual(await captured.json(), {owned: 1});
assert.deepEqual(await captured.json(), {owned: 1});
assert.equal(bodyCalls, 1, 'consumers reuse one native body capture');
assert.deepEqual(await owned.drain(performance.now() + 100), []);
assert.throws(() => owned.capture({body() { assert.fail('closed admission'); }}));

let resolveBody;
const delayed = createResponseBodyRegistry();
const delayedCapture = delayed.capture({body: () => new Promise(resolve => { resolveBody = resolve; })});
let consumed = false;
const consumption = delayedCapture.json().then(value => { consumed = true; return value; });
await Promise.resolve(); assert(!consumed, 'headers cannot complete body validation');
resolveBody(Buffer.from('{"delayed":true}'));
assert.deepEqual(await consumption, {delayed: true});
assert.deepEqual(await delayed.drain(performance.now() + 100), []);

const primary = new Error('controlled body rejection');
const consumedFailure = createResponseBodyRegistry();
const rejectedBody = consumedFailure.capture({body: () => Promise.reject(primary)});
await assert.rejects(rejectedBody.json(), error => error === primary);
assert.deepEqual(await consumedFailure.drain(performance.now() + 100), []);
const unconsumedFailure = createResponseBodyRegistry();
unconsumedFailure.capture({body() { throw primary; }});
assert.deepEqual(await unconsumedFailure.drain(performance.now() + 100), [primary]);

// Status remains the first assertion; an empty error body is not parsed first.
const statusFirst = createResponseBodyRegistry();
const empty = statusFirst.capture({body: async () => Buffer.from('')});
let parsed = false;
await assert.rejects((async () => {
    assert.equal(404, 200, 'HTTP status first'); parsed = true; await empty.json();
})(), error => error.code === 'ERR_ASSERTION' && error.message.includes('HTTP status first'));
assert(!parsed); assert.deepEqual(await statusFirst.drain(performance.now() + 100), []);

let release;
const pending = createResponseBodyRegistry();
pending.capture({body: () => new Promise(resolve => { release = resolve; })});
try { await assert.rejects(pending.drain(performance.now() + 5)); }
finally { release(Buffer.from('{}')); }
assert.deepEqual(await pending.drain(performance.now() + 100), [], 'timed-out join retains its owner');
const capped = createResponseBodyRegistry();
let started = 0;
for (let index = 0; index < 64; index++) capped.capture({body() { started++; return Promise.resolve(Buffer.from('{}')); }});
assert.throws(() => capped.capture({body() { started++; }}));
assert.equal(started, 64, 'overflow refuses before native capture');
assert.deepEqual(await capped.drain(performance.now() + 100), []);
'''
        subprocess.run(['node', '--input-type=module', '-e', script], cwd=repo / 'tests/browser',
                       check=True, timeout=10, capture_output=True, text=True)

    def test_background_tab_coordination_is_one_action_and_preserves_cleanup_failures(self):
        script = r'''
import assert from 'node:assert/strict';
import { withFrozenReadLane } from './installed-float-coordination.mjs';
const exercise = async ({actionError, freezeError, resumeError, drainError} = {}) => {
    const calls = [];
    const session = {send: async (_command, {state}) => {
        calls.push(state);
        if (state === 'frozen' && freezeError) throw freezeError;
        if (state === 'active' && resumeError) throw resumeError;
    }, detach: async () => { calls.push('detach'); }};
    const page = {context: () => ({newCDPSession: async () => session})};
    const lane = {drain: async (_deadline, options) => {
        assert.deepEqual(options, {uiBarrier: false}); calls.push('drain'); if (drainError) throw drainError;
    }};
    let value, error;
    try { value = await withFrozenReadLane(page, lane, performance.now() + 1000, async () => {
        calls.push('action'); if (actionError) throw actionError; return 200;
    }); } catch (caught) { error = caught; }
    return {calls, value, error};
};
let result = await exercise(); assert.equal(result.value, 200);
assert.deepEqual(result.calls, ['frozen', 'drain', 'action', 'active', 'detach']);
const primary = new Error('controlled primary'), cleanup = new Error('controlled cleanup');
result = await exercise({actionError: primary}); assert.equal(result.error, primary);
assert.deepEqual(result.calls, ['frozen', 'drain', 'action', 'active', 'detach']);
result = await exercise({actionError: primary, resumeError: cleanup});
assert.deepEqual(result.error.errors, [primary, cleanup]); assert.equal(result.calls.filter(x => x === 'action').length, 1);
result = await exercise({freezeError: primary}); assert.equal(result.error, primary);
assert.deepEqual(result.calls, ['frozen', 'active', 'detach']);
result = await exercise({drainError: primary}); assert.equal(result.error, primary);
assert.deepEqual(result.calls, ['frozen', 'drain', 'active', 'detach']);
// An actual expired action still resumes/detaches. Its timeout is not a
// successful mutation, and the finite cleanup budget does not renew it.
let calls = [], actions = 0;
const expiredSession = {send: async (_command, {state}) => { calls.push(state); },
    detach: async () => { calls.push('detach'); }};
const expiredPage = {context: () => ({newCDPSession: async () => expiredSession})};
await assert.rejects(withFrozenReadLane(expiredPage, {drain: async () => {}}, performance.now() + 5,
    () => { actions++; return new Promise(() => {}); }));
assert.equal(actions, 1); assert.deepEqual(calls, ['frozen', 'active', 'detach']);
// A resume ACK timeout must not precheck away the detach dispatch.
calls = [];
const slowResume = {send: async (_command, {state}) => {
    calls.push(state); if (state === 'active') return new Promise(() => {});
}, detach: async () => { calls.push('detach'); }};
try {
    await withFrozenReadLane({context: () => ({newCDPSession: async () => slowResume})},
        {drain: async () => {}}, performance.now() + 1000, () => { throw primary; });
    assert.fail('must preserve primary and cleanup timeout');
} catch (error) { assert.equal(error.errors[0], primary); assert.equal(error.errors.length, 2); }
assert.deepEqual(calls, ['frozen', 'active', 'detach']);
// Closing the owning context settles/owns a late session acquisition. Even a
// close failure cannot replace the original acquisition timeout.
for (const closeFails of [false, true]) {
    let resolve; const late = new Promise(yes => { resolve = yes; }); calls = [];
    const lateSession = {detach: async () => { calls.push('late-detach'); }};
    const context = {newCDPSession: () => late, close: async () => {
        calls.push('context-close'); resolve(lateSession); if (closeFails) throw cleanup;
    }};
    try {
        await withFrozenReadLane({context: () => context}, {drain: async () => {}}, performance.now() + 5,
            () => { actions++; });
        assert.fail('late acquisition cannot perform action');
    } catch (error) {
        if (closeFails) { assert(error instanceof AggregateError); assert.equal(error.errors[1], cleanup); }
        else assert(!(error instanceof AggregateError));
    }
    assert.deepEqual(calls, ['context-close', 'late-detach']);
}
assert.equal(actions, 1);
for (const rejected of [null, undefined, false, 0, '']) {
    const trace = [];
    const session = {send: async (_command, {state}) => { trace.push(state); },
        detach: async () => { trace.push('detach'); }};
    const page = {context: () => ({newCDPSession: async () => session})};
    let caught = false;
    try { await withFrozenReadLane(page, {drain: async () => {}}, performance.now() + 1000,
        () => { trace.push('action'); throw rejected; }); }
    catch (error) { caught = true; assert.equal(error, rejected); }
    assert(caught); assert.deepEqual(trace, ['frozen', 'action', 'active', 'detach']);
    session.send = async (_command, {state}) => { if (state === 'active') throw cleanup; };
    caught = false;
    try { await withFrozenReadLane(page, {drain: async () => {}}, performance.now() + 1000,
        () => { throw rejected; }); }
    catch (error) { caught = true; assert.deepEqual(error.errors, [rejected, cleanup]); }
    assert(caught);
}
'''
        subprocess.run(['node', '--input-type=module', '-e', script], cwd=repo / 'tests/browser',
                       check=True, timeout=10, capture_output=True, text=True)

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
