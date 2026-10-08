// Bounded shell-internal data only. Shape validation is not admission or a
// persistence approval, and reflective Proxy traps are not a browser sandbox.
export const FLOAT_LIMIT = 5;

const encoder = new TextEncoder();
const CONTEXT_LIMIT = 4096;
const ENTRY_LIMIT = 6144;
const SNAPSHOT_LIMIT = 32768;
const FORBIDDEN_KEYS = new Set(['__proto__', 'prototype', 'constructor']);
const DESCRIPTOR_KEYS = ['application_id', 'generation', 'panel_id', 'capability', 'context_key', 'context'];
const ENTRY_KEYS = ['application_id', 'panel_id', 'capability', 'context', 'presentation', 'maximized', 'geometry'];
const GEOMETRY_KEYS = ['x', 'y', 'width', 'height'];

function invalid() { throw new TypeError('invalid float data'); }
function guarded(operation) {
    try { return operation(); } catch { return invalid(); }
}
function bytes(value) { return encoder.encode(value).length; }
function matches(value, pattern) {
    return typeof value === 'string' && pattern.exec(value)?.[0] === value;
}
function ownRecord(value, exactKeys = null, maximum = SNAPSHOT_LIMIT) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) invalid();
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== null && prototype !== Object.prototype) invalid();
    const keys = Reflect.ownKeys(value);
    if (keys.length > maximum || keys.some(key => typeof key !== 'string')) invalid();
    if (exactKeys && (keys.length !== exactKeys.length || keys.some(key => !exactKeys.includes(key)))) invalid();
    const entries = keys.map(key => {
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        if (!descriptor || !Object.hasOwn(descriptor, 'value')) invalid();
        return [key, descriptor.value];
    });
    return entries;
}
function fields(value, keys) { return Object.fromEntries(ownRecord(value, keys, keys.length)); }
function ownArray(value, maximum) {
    if (!Array.isArray(value)) invalid();
    const lengthDescriptor = Object.getOwnPropertyDescriptor(value, 'length');
    if (!lengthDescriptor || !Object.hasOwn(lengthDescriptor, 'value')) invalid();
    const length = lengthDescriptor.value;
    if (!Number.isInteger(length) || length < 0 || length > maximum) invalid();
    const keys = Reflect.ownKeys(value);
    if (keys.length !== length + 1) invalid();
    const result = [];
    for (let index = 0; index < length; index++) {
        const descriptor = Object.getOwnPropertyDescriptor(value, String(index));
        if (!descriptor || !Object.hasOwn(descriptor, 'value')) invalid();
        result.push(descriptor.value);
    }
    return result;
}
function validString(value, minimum, maximum) {
    if (typeof value !== 'string' || value.includes('\u0000')) invalid();
    // TextEncoder replaces lone surrogates, so reject them before measuring.
    for (let index = 0; index < value.length; index++) {
        const code = value.charCodeAt(index);
        if (code >= 0xd800 && code <= 0xdbff) {
            const next = value.charCodeAt(++index);
            if (!(next >= 0xdc00 && next <= 0xdfff)) invalid();
        } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
    }
    const length = bytes(value);
    if (length < minimum || length > maximum) invalid();
    return value;
}
function compareBytes(left, right) {
    for (let index = 0; index < Math.min(left.length, right.length); index++) {
        if (left[index] !== right[index]) return left[index] - right[index];
    }
    return left.length - right.length;
}

// Only primitive JSON.stringify calls are used. Original decoded JSON is
// counted before filtering/normalization; no getter, toJSON or coercion hook
// is called. The bounded iterative walk also refuses cycles/non-JSON objects.
function dataJSON(value, limit) {
    const parts = [];
    let size = 0;
    const ancestors = new Set();
    const stack = [{ value }];
    const count = length => {
        size += length;
        if (size > limit) invalid();
    };
    const append = (text, counted = false) => {
        if (!counted) count(bytes(text));
        parts.push(text);
    };
    while (stack.length) {
        const item = stack.pop();
        if (Object.hasOwn(item, 'text')) { append(item.text, item.counted); continue; }
        if (Object.hasOwn(item, 'exit')) { ancestors.delete(item.exit); continue; }
        const current = item.value;
        if (current === null || typeof current === 'boolean' || typeof current === 'number') {
            if (typeof current === 'number' && !Number.isFinite(current)) invalid();
            append(JSON.stringify(current));
        } else if (typeof current === 'string') {
            if (bytes(current) > limit) invalid();
            append(JSON.stringify(current));
        } else if (current && typeof current === 'object') {
            if (ancestors.has(current)) invalid();
            ancestors.add(current);
            stack.push({ exit: current });
            if (Array.isArray(current)) {
                const entries = ownArray(current, Math.floor(limit / 2));
                // Charge all structural bytes before descending. Repeated
                // shared children cannot grow an unchecked pending stack.
                count(2 + Math.max(0, entries.length - 1));
                stack.push({ text: ']', counted: true });
                for (let index = entries.length - 1; index >= 0; index--) {
                    stack.push({ value: entries[index] });
                    if (index) stack.push({ text: ',', counted: true });
                }
                append('[', true);
            } else {
                const entries = ownRecord(current, null, Math.floor(limit / 4));
                count(2 + Math.max(0, entries.length - 1) + entries.length);
                const encodedKeys = entries.map(([key]) => {
                    if (bytes(key) > limit) invalid();
                    const encoded = JSON.stringify(key);
                    count(bytes(encoded));
                    return encoded;
                });
                stack.push({ text: '}', counted: true });
                for (let index = entries.length - 1; index >= 0; index--) {
                    const [, child] = entries[index];
                    stack.push({ value: child });
                    stack.push({ text: ':', counted: true });
                    stack.push({ text: encodedKeys[index], counted: true });
                    if (index) stack.push({ text: ',', counted: true });
                }
                append('{', true);
            }
        } else invalid();
    }
    return { json: parts.join(''), bytes: size };
}

export function canonicalCapability(value) {
    return guarded(() => {
        validString(value, 1, 256);
        const segments = value.split(':');
        if (segments.length !== 3) invalid();
        const [namespace, versionText, functionName] = segments;
        if (!matches(namespace, /^[a-z][a-z0-9_]{0,63}$/)
                || !matches(versionText, /^[0-9]+$/)
                || !matches(functionName, /^[a-z][a-z0-9_.]{0,127}$/)
                || functionName.endsWith('.') || functionName.includes('..')) invalid();
        const version = Number(versionText);
        if (!Number.isInteger(version) || version < 1 || version > 2147483647) invalid();
        return `${namespace}:${version}:${functionName}`;
    });
}

export function normalizeFloatContext(value) {
    return guarded(() => {
        const entries = ownRecord(value, null, 16).map(([key, child]) => {
            if (FORBIDDEN_KEYS.has(key)) invalid();
            validString(key, 1, 64);
            validString(child, 0, 512);
            return { key, value: child, encodedKey: encoder.encode(key) };
        }).sort((left, right) => compareBytes(left.encodedKey, right.encodedKey));
        const canonicalJSON = '{' + entries.map(entry =>
            JSON.stringify(entry.key) + ':' + JSON.stringify(entry.value)).join(',') + '}';
        const length = bytes(canonicalJSON);
        if (length > CONTEXT_LIMIT) invalid();
        const context = Object.create(null);
        for (const entry of entries) context[entry.key] = entry.value;
        return Object.freeze({ context: Object.freeze(context), canonicalJSON, bytes: length });
    });
}

export function copyPanelContext(normalizedContext) {
    return guarded(() => {
        const input = fields(normalizedContext, ['context', 'canonicalJSON', 'bytes']);
        const normalized = normalizeFloatContext(input.context);
        return Object.fromEntries(ownRecord(normalized.context, null, 16));
    });
}

function logicalIdentity(input) {
    if (!matches(input.application_id, /^[a-z][a-z0-9-]{1,63}$/)
            || !matches(input.panel_id, /^[a-z][a-z0-9_-]{0,63}$/)) invalid();
    return {
        application_id: input.application_id,
        panel_id: input.panel_id,
        capability: canonicalCapability(input.capability),
        context: normalizeFloatContext(input.context).context,
    };
}

export function normalizeFloatDescriptor(value) {
    return guarded(() => {
        const input = fields(value, DESCRIPTOR_KEYS);
        // Match launcher/model.js: generation is nonempty and <=256 codepoints,
        // not a novel ASCII or UTF-8-byte restriction.
        if (typeof input.generation !== 'string' || !input.generation.length
                || [...input.generation].length > 256) invalid();
        validString(input.context_key, 1, 512);
        return Object.freeze({ ...logicalIdentity(input), generation: input.generation,
            context_key: input.context_key });
    });
}

export function floatIdentityKey(value) {
    const descriptor = normalizeFloatDescriptor(value);
    return JSON.stringify([descriptor.application_id, descriptor.generation, descriptor.panel_id,
        descriptor.capability, descriptor.context_key, normalizeFloatContext(descriptor.context).canonicalJSON]);
}

function savedEntry(value) {
    if (dataJSON(value, ENTRY_LIMIT).bytes > ENTRY_LIMIT) invalid();
    const input = fields(value, ENTRY_KEYS);
    if (!['shown', 'minimized'].includes(input.presentation) || typeof input.maximized !== 'boolean') invalid();
    const geometry = fields(input.geometry, GEOMETRY_KEYS);
    for (const key of GEOMETRY_KEYS) {
        if (!Number.isInteger(geometry[key]) || geometry[key] < (key === 'x' || key === 'y' ? 0 : 1)
                || geometry[key] > 65535) invalid();
    }
    return Object.freeze({ ...logicalIdentity(input), presentation: input.presentation,
        maximized: input.maximized, geometry: Object.freeze(geometry) });
}

export function savedFloatIdentityKey(value) {
    return guarded(() => {
        const entry = savedEntry(value);
        return JSON.stringify([entry.application_id, entry.panel_id, entry.capability,
            normalizeFloatContext(entry.context).canonicalJSON]);
    });
}

function snapshotEntries(value) {
    dataJSON(value, SNAPSHOT_LIMIT);
    const root = fields(value, ['version', 'floats']);
    if (root.version !== 1) invalid();
    return ownArray(root.floats, FLOAT_LIMIT);
}

export function validateFloatSnapshot(value) {
    return guarded(() => {
        const seen = new Set();
        const entries = snapshotEntries(value).map(item => {
            const entry = savedEntry(item);
            const key = savedFloatIdentityKey(entry);
            if (seen.has(key)) invalid();
            seen.add(key);
            return entry;
        });
        const snapshot = Object.freeze({ version: 1, floats: Object.freeze(entries) });
        return Object.freeze({ value: snapshot, ...dataJSON(snapshot, SNAPSHOT_LIMIT) });
    });
}

export function normalizeSavedFloatSnapshot(value) {
    if (value === undefined) return Object.freeze({ entries: Object.freeze([]), warning: false });
    try {
        const seen = new Set();
        const entries = [];
        let warning = false;
        for (const item of snapshotEntries(value)) {
            try {
                const entry = savedEntry(item);
                const key = savedFloatIdentityKey(entry);
                if (seen.has(key)) { warning = true; continue; }
                seen.add(key);
                entries.push(entry);
            } catch { warning = true; }
        }
        return Object.freeze({ entries: Object.freeze(entries), warning });
    } catch {
        return Object.freeze({ entries: Object.freeze([]), warning: true });
    }
}

export function classifyFloatWorkArea(width, height) {
    return guarded(() => {
        if (typeof width !== 'number' || !Number.isFinite(width) || width < 0
                || typeof height !== 'number' || !Number.isFinite(height) || height < 0) invalid();
        width = Math.floor(width);
        height = Math.floor(height);
        const mode = !width || !height ? 'deferred' : width > 1024 ? 'desktop'
            : width >= 768 ? 'slide-over' : 'full-modal';
        return Object.freeze({ width, height, mode });
    });
}
function workArea(value) {
    const input = ownRecord(value, null, 3);
    const fields = Object.fromEntries(input);
    if (input.some(([key]) => !['width', 'height', 'mode'].includes(key))) invalid();
    return classifyFloatWorkArea(fields.width, fields.height);
}
function integerGeometry(value) {
    const geometry = fields(value, GEOMETRY_KEYS);
    if (GEOMETRY_KEYS.some(key => !Number.isInteger(geometry[key]))) invalid();
    return geometry;
}
function clamp(value, minimum, maximum) { return Math.min(maximum, Math.max(minimum, value)); }

export function clampFloatGeometry(value, area) {
    return guarded(() => {
        const geometry = integerGeometry(value);
        const { width: W, height: H, mode } = workArea(area);
        if (mode === 'deferred') return null;
        const width = clamp(geometry.width, Math.min(W, 320), W);
        const height = clamp(geometry.height, Math.min(H, 240), H);
        return Object.freeze({ x: clamp(geometry.x, 0, W - width), y: clamp(geometry.y, 0, H - height), width, height });
    });
}

export function defaultFloatGeometry(area, rank) {
    return guarded(() => {
        if (!Number.isInteger(rank) || rank < 0 || rank >= FLOAT_LIMIT) invalid();
        const { width: W, height: H, mode } = workArea(area);
        if (mode === 'deferred') return null;
        const width = Math.min(W, 640), height = Math.min(H, 480);
        return clampFloatGeometry({ x: Math.floor((W - width) / 2) + 24 * rank,
            y: Math.floor((H - height) / 2) + 24 * rank, width, height }, area);
    });
}

export function saveFloatGeometry(value) {
    return guarded(() => {
        const geometry = integerGeometry(value);
        return Object.freeze({ x: clamp(geometry.x, 0, 65535), y: clamp(geometry.y, 0, 65535),
            width: clamp(geometry.width, 1, 65535), height: clamp(geometry.height, 1, 65535) });
    });
}
