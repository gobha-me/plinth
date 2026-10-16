import assert from 'node:assert/strict';
import test from 'node:test';
import { decodeFloatTarget, decodeFloatCatalog, decodeFloatAdmission, decodePersonalFloatDefaults,
    decodeFloatLayout, encodeFloatLayout, parseFloatJSON, copyFloatJSON, canonicalContentType,
    positiveUint64, validateFloatAdmissionRequests, projectFloatContext, validateFloatContext,
    pinnedFloatRequest } from '../../client/shell/client/panels/float-wire.js';
import { target, entry, snapshot, location, generation, artifact, plain } from './float-production-fixtures.mjs';
const options = { location };
const decode = value => decodeFloatTarget(value, options);

test('typed wire exact metadata and native immutable namespace are mandatory', () => {
    const raw = target(), valid = decode(raw);
    assert.equal(valid.module_url, location.origin + raw.module_url);
    assert.equal(valid.persistence_contract.policy_revision, '1');
    for (const override of [{ extra: true }, { kind: 'float' }, { type: 'primary' },
        { generation: generation.toUpperCase() }, { title: 'x'.repeat(257) }, { title: '\ud800' },
        { version: 'é'.repeat(129) }, { logical_install_order: '01' }, { icon: null },
        { capability: 'notes:01:preview' }, { content_types: ['Text/Plain'] },
        { content_types: ['text/plain', 'text/plain'] }]) {
        assert.throws(() => decode(target(override)));
    }
    assert.equal(decode(target({ title: '😀'.repeat(256), version: 'é'.repeat(128) })).title.length, 512);
    assert.deepEqual(decode(target({ content_types: [] })).content_types, []);
    for (const path of ['/ext/notes/client/panel.js', raw.module_url + '?token=fake', raw.module_url + '#x',
        location.origin + raw.module_url, raw.module_url.replace(generation, 'ffffffff-ffff-ffff-ffff-ffffffffffff'),
        raw.module_url.replace('/preview/', '/other/'), raw.module_url.replace('panel.js', '%70anel.js'),
        raw.module_url.replace('panel.js', '%2Fpanel.js'), raw.module_url.replace('panel.js', '..%2Fpanel.js'),
        raw.module_url.replace('panel.js', '%00.js'), raw.module_url.replace('panel.js', 'a//b.js')]) {
        assert.throws(() => decode(target({ module_url: path })), path);
    }
});

test('typed context meaning NUL survives but context NUL/getters/unknown fields do not', () => {
    const raw = target(); raw.context_schema.fields.record_id.meaning = 'stable\0meaning';
    const decoded = decode(raw);
    assert.equal(decoded.context_schema.fields.record_id.meaning, 'stable\0meaning');
    assert.deepEqual(plain(validateFloatContext({ record_id: 'fake' }, decoded.context_schema).context), { record_id: 'fake' });
    for (const context of [undefined, {}, { record_id: 'x'.repeat(33) }, { record_id: 'a\0b' },
        { record_id: 'fake', extra: 'no' }, { record_id: ['no'] }]) {
        assert.throws(() => validateFloatContext(context, decoded.context_schema));
    }
    let invoked = 0;
    const accessor = Object.defineProperty({}, 'record_id', { enumerable: true, get() { invoked++; return 'fake'; } });
    assert.throws(() => validateFloatContext(accessor, decoded.context_schema));
    const hostile = target(); Object.defineProperty(hostile.context_schema.fields.record_id, 'meaning', {
        get() { invoked++; return 'bad'; } });
    assert.throws(() => decode(hostile)); assert.equal(invoked, 0);
    assert.equal(projectFloatContext({ record_id: 'fake' }, decode(target({ persistence_contract: null }))), null);
    const missing = target(); missing.persistence_contract.approved_keys = [];
    assert.throws(() => decode(missing));
    assert.equal(projectFloatContext({ record_id: 'fake' }, missing), null);
    assert.deepEqual(plain(projectFloatContext({ record_id: 'fake' }, decoded)), { record_id: 'fake' });
});

test('bounded parser preserves duplicate evidence and never invokes data hooks', () => {
    assert.equal(parseFloatJSON('{"meaning":"a\\u0000b"}').meaning, 'a\0b');
    for (const raw of ['{"x":1,"\\u0078":2}', '{"nested":{"a":1,"a":2}}', '[1,]', '{"x":1,}',
        '{}x', '01', '1e999', '"\\ud800"', '\ufeff{}', '['.repeat(22) + '0' + ']'.repeat(22)]) {
        assert.throws(() => parseFloatJSON(raw), raw);
    }
    assert.throws(() => parseFloatJSON('"éé"', 5));
    let invoked = 0;
    const hostile = { toJSON() { invoked++; return {}; } };
    assert.throws(() => copyFloatJSON(hostile));
    const accessor = Object.defineProperty({}, 'x', { enumerable: true, get() { invoked++; } });
    assert.throws(() => copyFloatJSON(accessor)); assert.equal(invoked, 0);
    const cyclic = {}; cyclic.self = cyclic; assert.throws(() => copyFloatJSON(cyclic));
    assert.throws(() => copyFloatJSON(Array(8192).fill('x'.repeat(32760))));
    assert.deepEqual(plain(copyFloatJSON({ x: 'é' }, 10)), { x: 'é' });
    assert.throws(() => copyFloatJSON({ x: 'é' }, 9));
    assert.deepEqual(plain(copyFloatJSON({ value: [true, null, 'é'] })), { value: [true, null, 'é'] });
});

test('catalog rank ordering is uint64 exact and administrator field is request-specific', () => {
    assert.equal(positiveUint64('18446744073709551615'), '18446744073709551615');
    for (const rank of ['0', '01', '18446744073709551616', 1]) assert.throws(() => positiveUint64(rank));
    assert.equal(canonicalContentType('Text/Plain'), 'text/plain');
    for (const type of [' text/plain', 'text/*', 'text/plain;charset=x', 'text/plain\n']) assert.throws(() => canonicalContentType(type));
    const page = { schema_version: 1, kind: 'float', targets: [target({ logical_install_order: '9007199254740992' }),
        target({ application_id: 'other', logical_install_order: '9007199254740993' })], next_cursor: null };
    assert.equal(decodeFloatCatalog(page, { limit: 64 }, options).targets.length, 2);
    assert.throws(() => decodeFloatCatalog({ ...page, targets: [...page.targets].reverse() }, { limit: 64 }, options));
    assert.throws(() => decodeFloatCatalog({ ...page, admin_default_candidate: null }, {}, options));
    assert.throws(() => decodeFloatCatalog(page, { include_admin_default: true }, options));
    assert.equal(decodeFloatCatalog({ ...page, admin_default_candidate: null },
        { include_admin_default: true }, options).admin_default_candidate, null);
});

test('admission rejects partial duplicate mixed and forged correspondence before returning data', () => {
    assert.deepEqual(validateFloatAdmissionRequests([]), []);
    assert.deepEqual(decodeFloatAdmission({ schema_version: 1, results: [] }, [], options), []);
    const requests = validateFloatAdmissionRequests([pinnedFloatRequest(target()),
        { application_id: 'other', panel_id: 'preview', kind: 'float', expected_generation: null, content_type: 'text/plain' }]);
    const response = { schema_version: 1, results: [{ index: 1, status: 'unavailable' },
        { index: 0, status: 'available', target: target() }] };
    assert.equal(decodeFloatAdmission(response, requests, options)[0].target.application_id, 'notes');
    for (const results of [response.results.slice(1), [response.results[1], response.results[1]],
        [{ index: 0, status: 'available', target: target({ application_id: 'forged' }) }, response.results[0]],
        [{ index: 0, status: 'stale', target: null }, response.results[0]],
        [{ index: 0, status: 'available', target: target({ capability: 'notes:1:other' }) }, response.results[0]]]) {
        assert.throws(() => decodeFloatAdmission({ schema_version: 1, results }, requests, options));
    }
    assert.throws(() => validateFloatAdmissionRequests([requests[0], requests[0]]));
    assert.throws(() => validateFloatAdmissionRequests([{ ...requests[0], content_type: 'text/plain' }]));
    assert.throws(() => validateFloatAdmissionRequests(Array(39).fill(requests[0])));
});

test('personal defaults preserve absent-versus-null and never rewrite malformed data', () => {
    assert.deepEqual(decodePersonalFloatDefaults({}), []);
    const choice = { content_type: 'text/plain', application_id: 'notes', panel_id: 'preview' };
    assert.equal(decodePersonalFloatDefaults({ value: { version: 1, choices: [choice] } }).length, 1);
    for (const value of [null, { version: 2, choices: [] }, { version: 1, choices: [choice, choice] },
        { version: 1, choices: [{ ...choice, content_type: 'Text/Plain' }] }]) {
        assert.throws(() => decodePersonalFloatDefaults({ value }));
    }
});

test('layout exact optional fields original indexes and local-free commit preconditions', () => {
    const raw = { schema_version: 1, present: true, layout: snapshot(entry(), null, entry('last')),
        policy_revision: '1', restore_targets: [{ index: 2, status: 'available', target: target() },
            { index: 0, status: 'available', target: target() }, { index: 1, status: 'unavailable' }] };
    assert.deepEqual(decodeFloatLayout(raw, options).restore_targets.map(item => item.index), [2, 0, 1]);
    assert.throws(() => decodeFloatLayout({ ...raw, restore_targets: raw.restore_targets.slice(1) }, options));
    assert.throws(() => decodeFloatLayout({ ...raw, policy_revision: null }, options));
    assert.equal(decodeFloatLayout({ schema_version: 1, present: true, layout: null,
        policy_revision: null, restore_targets: [] }, options).present, true);
    assert.throws(() => decodeFloatLayout({ schema_version: 1, present: false, layout: null,
        policy_revision: null, restore_targets: [] }, options));
    const binding = { index: 0, generation, artifact_digest: artifact, schema_digest: 'b'.repeat(64),
        projection_contract: { id: 'record', version: 1 }, policy_revision: '1' };
    const body = JSON.parse(encodeFloatLayout(snapshot(entry()), [binding]));
    assert.deepEqual(Object.keys(body), ['schema_version', 'layout', 'bindings']);
    assert.equal(body.bindings[0].generation, generation);
    assert.throws(() => encodeFloatLayout(snapshot(entry()), []));
    assert.throws(() => encodeFloatLayout(snapshot(entry()), [{ ...binding, local_revision: 'no' }]));
    assert.deepEqual(JSON.parse(encodeFloatLayout(snapshot(), [])), { schema_version: 1, layout: snapshot(), bindings: [] });
});
