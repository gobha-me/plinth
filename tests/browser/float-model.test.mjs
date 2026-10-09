import assert from 'node:assert/strict';
import test from 'node:test';
import {
    FLOAT_LIMIT, canonicalCapability, normalizeFloatContext, copyPanelContext,
    normalizeFloatDescriptor, floatIdentityKey, savedFloatIdentityKey,
    validateFloatSnapshot, normalizeSavedFloatSnapshot, classifyFloatWorkArea,
    defaultFloatGeometry, clampFloatGeometry, saveFloatGeometry,
} from '../../client/shell/client/panels/float-model.js';

const encoder = new TextEncoder();
const descriptor = (changes = {}) => ({ application_id: 'notes', generation: 'generation-one',
    panel_id: 'preview', capability: 'notes:1:preview', context_key: 'key', context: {}, ...changes });
const entry = (changes = {}) => ({ application_id: 'notes', panel_id: 'preview',
    capability: 'notes:1:preview', context: { record_id: 'fake-record' }, presentation: 'shown',
    maximized: false, geometry: { x: 24, y: 24, width: 640, height: 480 }, ...changes });
const snapshot = floats => ({ version: 1, floats });
const invalid = operation => assert.throws(operation, error =>
    error instanceof TypeError && error.message === 'invalid float data');

test('context canonicalization orders integer-like keys by UTF-8 bytes', () => {
    const actual = normalizeFloatContext({ 2: 'two', 10: 'ten', 1: 'one', '01': 'zero-one' });
    assert.equal(actual.canonicalJSON, '{"01":"zero-one","1":"one","10":"ten","2":"two"}');
    assert.equal(actual.bytes, encoder.encode(actual.canonicalJSON).length);
    assert(Object.isFrozen(actual.context));
});

test('context canonicalization orders astral and BMP keys independently of UTF-16', () => {
    const actual = normalizeFloatContext({ '\u{10000}': 'astral', '\ue000': 'bmp', a: 'ascii' });
    assert.equal(actual.canonicalJSON, '{"a":"ascii","\ue000":"bmp","\u{10000}":"astral"}');
});

test('context UTF-8 key and value limits accept boundaries and reject excess', () => {
    const key = 'a'.repeat(60) + '😀';
    assert.equal(normalizeFloatContext({ [key]: 'é'.repeat(256) }).context[key], 'é'.repeat(256));
    invalid(() => normalizeFloatContext({ [key + 'a']: '' }));
    invalid(() => normalizeFloatContext({ a: 'é'.repeat(257) }));
    invalid(() => normalizeFloatContext({ '': 'empty-key' }));
    assert.equal(normalizeFloatContext({ a: '' }).context.a, '');
});

test('context canonical JSON accepts 4096 bytes and rejects 4097 bytes', () => {
    const value = Object.fromEntries(Array.from({ length: 8 }, (_, index) => ['k' + index, 'x'.repeat(512)]));
    value.k7 = value.k7.slice(0, 512 - (encoder.encode(JSON.stringify(value)).length - 4096));
    assert.equal(normalizeFloatContext(value).bytes, 4096);
    value.k7 += 'x';
    invalid(() => normalizeFloatContext(value));
});

test('context rejects excess keys, arrays, nested values and forbidden keys', () => {
    assert.equal(Object.keys(normalizeFloatContext(Object.fromEntries(
        Array.from({ length: 16 }, (_, index) => ['k' + index, '']))).context).length, 16);
    invalid(() => normalizeFloatContext(Object.fromEntries(
        Array.from({ length: 17 }, (_, index) => ['k' + index, '']))));
    for (const value of [[], null, { a: {} }, { a: 1 }, { a: false }, { a: undefined }, new Date()]) {
        invalid(() => normalizeFloatContext(value));
    }
    for (const key of ['__proto__', 'prototype', 'constructor']) {
        const value = Object.create(null); value[key] = 'no';
        invalid(() => normalizeFloatContext(value));
    }
    const symbols = { a: '' }; symbols[Symbol()] = 'no';
    invalid(() => normalizeFloatContext(symbols));
    const plain = Object.create(null); plain.a = 'yes';
    assert.equal(normalizeFloatContext(plain).context.a, 'yes');
});

test('context rejects NUL and lone surrogates without repairing strings', () => {
    for (const value of ['\u0000', '\ud800', '\udc00', '\ud800a', 'a\udc00']) {
        invalid(() => normalizeFloatContext({ [value]: '' }));
        invalid(() => normalizeFloatContext({ a: value }));
    }
    assert.equal(normalizeFloatContext({ emoji: '😀' }).context.emoji, '😀');
});

test('ordinary accessor and serialization hooks are not invoked', () => {
    let calls = 0;
    const accessor = {}; Object.defineProperty(accessor, 'a', { get() { calls++; return 'no'; } });
    invalid(() => normalizeFloatContext(accessor));
    invalid(() => normalizeFloatContext({ toJSON() { calls++; return {}; } }));
    invalid(() => normalizeFloatContext({ a: { toString() { calls++; return 'no'; } } }));
    const root = snapshot([entry()]);
    Object.defineProperty(root, 'toJSON', { value() { calls++; return snapshot([]); } });
    invalid(() => validateFloatSnapshot(root));
    const normalizedWrapper = {}; Object.defineProperty(normalizedWrapper, 'context', {
        get() { calls++; return {}; } });
    invalid(() => copyPanelContext(normalizedWrapper));
    assert.equal(calls, 0);
});

test('throwing reflective Proxy traps are refused without a sandbox claim', () => {
    let calls = 0;
    const value = new Proxy({}, { ownKeys() { calls++; throw new Error('private details'); } });
    invalid(() => normalizeFloatContext(value));
    assert.equal(calls, 1, 'reflection can execute a Proxy trap; no trap-suppression claim');
    assert.deepEqual(normalizeSavedFloatSnapshot(value), { entries: [], warning: true });
});

test('panel context mutation cannot alter owned context or dedup identity', () => {
    const source = { record_id: 'original' };
    const normalized = normalizeFloatContext(source);
    const panel = copyPanelContext(normalized);
    const owned = normalizeFloatDescriptor(descriptor({ context: source }));
    const key = floatIdentityKey(owned);
    source.record_id = 'source-mutated'; panel.record_id = 'panel-mutated'; panel.extra = 'new';
    assert.equal(normalized.context.record_id, 'original');
    assert.equal(owned.context.record_id, 'original');
    assert.equal(floatIdentityKey(owned), key);
    assert.notEqual(panel, source);
    assert.notEqual(panel, normalized.context);
});

test('capability decimal spellings normalize to one registry identity', () => {
    assert.equal(canonicalCapability('notes:0001:preview'), 'notes:1:preview');
    assert.equal(floatIdentityKey(descriptor({ capability: 'notes:0001:preview' })),
        floatIdentityKey(descriptor()));
    assert.equal(canonicalCapability('notes:2147483647:preview'), 'notes:2147483647:preview');
});

test('capability validation matches namespace, function, version and byte bounds', () => {
    assert.equal(canonicalCapability('a:1:b.c_d'), 'a:1:b.c_d');
    assert.equal(canonicalCapability('a'.repeat(64) + ':1:' + 'b'.repeat(128)),
        'a'.repeat(64) + ':1:' + 'b'.repeat(128));
    for (const value of ['notes:preview', 'notes:1:preview:extra', ':1:preview', 'notes::preview',
        'notes:1:', 'notes:0:preview', 'notes:2147483648:preview', 'notes:+1:preview',
        'notes:-1:preview', 'notes:1.0:preview', 'notes:1e0:preview', 'notes: 1:preview',
        'notes:1:preview\n', 'Notes:1:preview', 'no-tes:1:preview', 'notes:1:.preview',
        'notes:1:preview.', 'notes:1:a..b', 'notes:1:a-b', 'a'.repeat(65) + ':1:b',
        'a:1:' + 'b'.repeat(129)]) invalid(() => canonicalCapability(value));
    const boundary = 'a:' + '0'.repeat(251) + '1:b';
    assert.equal(boundary.length, 256);
    assert.equal(canonicalCapability(boundary), 'a:1:b');
    invalid(() => canonicalCapability('a:' + '0'.repeat(252) + '1:b'));
});

test('descriptor IDs, generation and context key are strictly bounded own data', () => {
    assert.equal(normalizeFloatDescriptor(descriptor({ generation: '😀'.repeat(256),
        context_key: 'é'.repeat(256) })).generation, '😀'.repeat(256));
    assert.equal(normalizeFloatDescriptor(descriptor({ generation: 'a\u0000b' })).generation, 'a\u0000b');
    for (const changes of [{ generation: '' }, { generation: '😀'.repeat(257) },
        { generation: 1 }, { context_key: '' }, { context_key: 'é'.repeat(257) },
        { context_key: '\u0000' }, { application_id: 'a' }, { application_id: 'notes\n' },
        { application_id: 'a'.repeat(65) }, { panel_id: 'preview\n' }, { panel_id: 'a'.repeat(65) },
        { extra: 'no' }]) invalid(() => normalizeFloatDescriptor(descriptor(changes)));
    const value = descriptor(); Object.defineProperty(value, 'generation', { get() { throw new Error('no'); } });
    invalid(() => normalizeFloatDescriptor(value));
});

test('identity encoding distinguishes delimiter ambiguities and context-key collisions', () => {
    const first = descriptor({ generation: 'a\u0000b', context: { a: 'one' }, context_key: 'collision' });
    const second = descriptor({ generation: 'a\u0000b', context: { a: 'two' }, context_key: 'collision' });
    assert.notEqual(floatIdentityKey(first), floatIdentityKey(second));
    assert.equal(JSON.parse(floatIdentityKey(first)).length, 6);
    assert.equal(floatIdentityKey(descriptor({ context: { b: 'two', a: 'one' } })),
        floatIdentityKey(descriptor({ context: { a: 'one', b: 'two' } })));
    assert.equal(savedFloatIdentityKey(entry({ capability: 'notes:01:preview' })), savedFloatIdentityKey(entry()));
});

test('snapshot writes reject unknown fields, malformed geometry and duplicates', () => {
    const valid = validateFloatSnapshot(snapshot([entry({ presentation: 'minimized', maximized: true })]));
    assert.equal(valid.bytes, encoder.encode(valid.json).length);
    assert.equal(JSON.parse(valid.json).floats[0].presentation, 'minimized');
    for (const changes of [{ extra: 'no' }, { module_url: '/ext/no' }, { presentation: 'hidden' },
        { maximized: 1 }, { geometry: { x: -1, y: 0, width: 1, height: 1 } },
        { geometry: { x: 0, y: 0, width: 0, height: 1 } },
        { geometry: { x: 65536, y: 0, width: 1, height: 1 } },
        { geometry: { x: 0, y: 0, width: 1.5, height: 1 } }]) {
        invalid(() => validateFloatSnapshot(snapshot([entry(changes)])));
    }
    invalid(() => validateFloatSnapshot(snapshot([entry(), entry({ capability: 'notes:01:preview' })])));
    invalid(() => validateFloatSnapshot({ ...snapshot([]), extra: 'no' }));
    assert.equal(validateFloatSnapshot(snapshot([entry({ geometry: { x: 65535, y: 65535,
        width: 65535, height: 65535 } })])).value.floats.length, 1);
});

test('saved reads distinguish absence and empty from malformed root envelopes', () => {
    assert.deepEqual(normalizeSavedFloatSnapshot(undefined), { entries: [], warning: false });
    assert.deepEqual(normalizeSavedFloatSnapshot(snapshot([])), { entries: [], warning: false });
    for (const value of [null, [], 'json', { version: 2, floats: [] }, { version: 1 },
        { version: 1, floats: {} }, { version: 1, floats: Array(6).fill(entry()) },
        { version: 1, floats: [], extra: 'no' }]) {
        assert.deepEqual(normalizeSavedFloatSnapshot(value), { entries: [], warning: true });
    }
    assert.equal(FLOAT_LIMIT, 5);
});

test('saved reads skip malformed entries and retain the first valid duplicate', () => {
    const source = snapshot([entry({ presentation: 'minimized' }), null,
        entry({ capability: 'notes:01:preview', presentation: 'shown' }),
        entry({ context: { record_id: 'another' } })]);
    const before = JSON.stringify(source);
    const result = normalizeSavedFloatSnapshot(source);
    assert.equal(result.warning, true);
    assert.equal(result.entries.length, 2);
    assert.equal(result.entries[0].presentation, 'minimized');
    assert.equal(result.entries[1].context.record_id, 'another');
    assert.equal(JSON.stringify(source), before, 'normalization never repairs or overwrites the original');
});

test('original saved entry and aggregate sizes are measured before filtering', () => {
    const smallMalformed = entry({ extra: 'x'.repeat(7000) });
    assert.equal(normalizeSavedFloatSnapshot(snapshot([smallMalformed, entry()])).entries.length, 1);
    const oversizedRoot = snapshot([entry({ extra: 'x'.repeat(32768) }), entry()]);
    assert.deepEqual(normalizeSavedFloatSnapshot(oversizedRoot), { entries: [], warning: true });
    invalid(() => validateFloatSnapshot(oversizedRoot));
    const cycle = {}; cycle.self = cycle;
    assert.deepEqual(normalizeSavedFloatSnapshot(snapshot([cycle])), { entries: [], warning: true });
    let shared = { leaf: 'value' };
    for (let depth = 0; depth < 20; depth++) shared = { a: shared, b: shared };
    assert.deepEqual(normalizeSavedFloatSnapshot(snapshot([shared])), { entries: [], warning: true },
        'repeated aliased subtrees stop at the original byte bound');
});

test('work-area classification covers flooring, zero area and 768/1024 edges', () => {
    for (const [width, mode] of [[767.99, 'full-modal'], [768, 'slide-over'],
        [1024.99, 'slide-over'], [1025, 'desktop'], [0.99, 'deferred']]) {
        assert.deepEqual(classifyFloatWorkArea(width, 480.9), { width: Math.floor(width), height: 480, mode });
    }
    assert.equal(classifyFloatWorkArea(1200, 0.99).mode, 'deferred');
    for (const dimensions of [[-1, 1], [1, Infinity], [NaN, 1], ['768', 1]]) {
        invalid(() => classifyFloatWorkArea(...dimensions));
    }
});

test('geometry defaults, clamps and saved saturation preserve finite integer bounds', () => {
    assert.deepEqual(defaultFloatGeometry({ width: 1200, height: 800 }, 0),
        { x: 280, y: 160, width: 640, height: 480 });
    assert.deepEqual(defaultFloatGeometry({ width: 1200, height: 800 }, 4),
        { x: 376, y: 256, width: 640, height: 480 });
    assert.deepEqual(clampFloatGeometry({ x: -10, y: 900, width: 9999, height: 1 },
        { width: 320, height: 80 }), { x: 0, y: 0, width: 320, height: 80 });
    assert.equal(defaultFloatGeometry({ width: 0, height: 800 }, 0), null);
    assert.equal(clampFloatGeometry({ x: 0, y: 0, width: 10, height: 10 }, { width: 10, height: 0 }), null);
    assert.deepEqual(saveFloatGeometry({ x: -1, y: 99999, width: 99999, height: -1 }),
        { x: 0, y: 65535, width: 65535, height: 1 });
    invalid(() => defaultFloatGeometry({ width: 1200, height: 800 }, 5));
    invalid(() => saveFloatGeometry({ x: 0.5, y: 0, width: 1, height: 1 }));
});
