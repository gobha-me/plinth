// Strict shell-internal typed-float wire data. Decoding is never authority.
import { canonicalCapability, isFloatMetadataString, normalizeFloatContext,
    validateFloatSnapshot } from './float-model.js';

const encoder = new TextEncoder();
const FORBIDDEN = new Set(['__proto__', 'prototype', 'constructor']);
const APP = /^[a-z][a-z0-9-]{1,63}$/;
const PANEL = /^[a-z][a-z0-9_-]{0,63}$/;
const TOKEN = /^[a-z][a-z0-9-]{0,63}$/;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
export function invalidFloatWire() { throw new TypeError('invalid float response'); }
export const utf8Bytes = value => encoder.encode(value).length;
export function copyFloatJSON(value, maximum = 32768) {
    let nodes = 0, bytes = 0;
    const ancestors = new Set();
    const charge = amount => { bytes += amount; if (bytes > maximum) invalidFloatWire(); };
    function copy(item, depth) {
        if (++nodes > 65536 || depth > 20) invalidFloatWire();
        if (item === null || typeof item === 'boolean' ||
            typeof item === 'number' && Number.isFinite(item)) {
            charge(utf8Bytes(JSON.stringify(item))); return item;
        }
        if (typeof item === 'string') {
            const result = text(item, maximum, { empty: true });
            charge(utf8Bytes(JSON.stringify(result))); return result;
        }
        if (!item || typeof item !== 'object' || ancestors.has(item)) invalidFloatWire();
        ancestors.add(item);
        let result;
        if (Array.isArray(item)) {
            const children = wireArray(item, Math.floor(maximum / 2));
            charge(2 + Math.max(0, children.length - 1));
            result = children.map(child => copy(child, depth + 1));
        }
        else {
            result = Object.create(null);
            const entries = Object.entries(wireRecord(item, null, Math.floor(maximum / 4)));
            charge(2 + Math.max(0, entries.length - 1) + entries.length);
            for (const [name, child] of entries) {
                text(name, maximum, { empty: true });
                charge(utf8Bytes(JSON.stringify(name)));
                Object.defineProperty(result, name, { enumerable: true, value: copy(child, depth + 1) });
            }
        }
        ancestors.delete(item);
        return result;
    }
    return copy(value, 0);
}
export function wireRecord(value, keys = null, maximum = 64) {
    if (!value || typeof value !== 'object' || Array.isArray(value) ||
        ![Object.prototype, null].includes(Object.getPrototypeOf(value))) invalidFloatWire();
    const names = Reflect.ownKeys(value);
    if (names.length > maximum || names.some(key => typeof key !== 'string') ||
        (keys && (names.length !== keys.length || names.some(key => !keys.includes(key))))) invalidFloatWire();
    const result = Object.create(null);
    for (const key of names) {
        const property = Object.getOwnPropertyDescriptor(value, key);
        if (!property || !Object.hasOwn(property, 'value')) invalidFloatWire();
        Object.defineProperty(result, key, { enumerable: true, value: property.value });
    }
    return result;
}
export function wireArray(value, maximum) {
    if (!Array.isArray(value) || Object.getPrototypeOf(value) !== Array.prototype) invalidFloatWire();
    const length = Object.getOwnPropertyDescriptor(value, 'length')?.value;
    if (!Number.isInteger(length) || length < 0 || length > maximum ||
        Reflect.ownKeys(value).length !== length + 1) invalidFloatWire();
    return Array.from({ length }, (_, index) => {
        const property = Object.getOwnPropertyDescriptor(value, String(index));
        if (!property || !Object.hasOwn(property, 'value')) invalidFloatWire();
        return property.value;
    });
}
function text(value, maximum, { empty = false, nul = true, scalars = maximum } = {}) {
    if (empty && value === '') return value;
    if (!isFloatMetadataString(value, maximum, scalars) || (!nul && value.includes('\0'))) invalidFloatWire();
    return value;
}
function match(value, expression) {
    if (typeof value !== 'string' || expression.exec(value)?.[0] !== value) invalidFloatWire();
    return value;
}
export function logicalFloat(value) {
    const data = wireRecord(value, ['application_id', 'panel_id']);
    return Object.freeze({ application_id: match(data.application_id, APP), panel_id: match(data.panel_id, PANEL) });
}
export function canonicalContentType(value) {
    text(value, 128, { nul: false });
    return match(value, /^[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*\/[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*$/).toLowerCase();
}
export function positiveUint64(value) {
    match(value, /^[1-9][0-9]{0,19}$/);
    if (value.length === 20 && value > '18446744073709551615') invalidFloatWire();
    return value;
}
function capability(value) {
    text(value, 256);
    if (canonicalCapability(value) !== value) invalidFloatWire();
    return value;
}
function digest(value) { return match(value, /^[0-9a-f]{64}$/); }
function key(value) {
    text(value, 64, { nul: false });
    if (FORBIDDEN.has(value)) invalidFloatWire();
    return value;
}
export function decodeContextSchema(value) {
    const data = wireRecord(value, ['version', 'fields', 'persist_keys']);
    if (data.version !== 1) invalidFloatWire();
    const fields = Object.create(null);
    for (const [name, raw] of Object.entries(wireRecord(data.fields, null, 16))) {
        key(name);
        const field = wireRecord(raw, ['required', 'max_bytes', 'meaning']);
        if (typeof field.required !== 'boolean' || !Number.isInteger(field.max_bytes) ||
            field.max_bytes < 0 || field.max_bytes > 512) invalidFloatWire();
        fields[name] = Object.freeze({ required: field.required, max_bytes: field.max_bytes,
            meaning: text(field.meaning, 256) });
    }
    const persist = wireArray(data.persist_keys, 16).map(key);
    if (new Set(persist).size !== persist.length || persist.some(name => !Object.hasOwn(fields, name))) invalidFloatWire();
    return Object.freeze({ version: 1, fields: Object.freeze(fields), persist_keys: Object.freeze(persist) });
}
export function validateFloatContext(value, schema) {
    const normalized = normalizeFloatContext(value === undefined ? {} : value);
    for (const [name, child] of Object.entries(normalized.context)) {
        if (!Object.hasOwn(schema.fields, name) || utf8Bytes(child) > schema.fields[name].max_bytes) invalidFloatWire();
    }
    if (Object.entries(schema.fields).some(([name, field]) => field.required &&
        !Object.hasOwn(normalized.context, name))) invalidFloatWire();
    return normalized;
}
export function decodePersistenceContract(value, schema) {
    if (value === null) return null;
    const data = wireRecord(value, ['projection_contract', 'artifact_digest', 'schema_digest',
        'approved_keys', 'policy_revision']);
    const contract = wireRecord(data.projection_contract, ['id', 'version']);
    match(contract.id, PANEL);
    if (!Number.isSafeInteger(contract.version) || contract.version < 1) invalidFloatWire();
    const approved = wireArray(data.approved_keys, 16).map(key);
    if (new Set(approved).size !== approved.length || approved.some(name => !Object.hasOwn(schema.fields, name) ||
        !schema.persist_keys.includes(name)) || Object.entries(schema.fields).some(([name, field]) =>
        field.required && !approved.includes(name))) invalidFloatWire();
    return Object.freeze({ projection_contract: Object.freeze({ id: contract.id, version: contract.version }),
        artifact_digest: digest(data.artifact_digest), schema_digest: digest(data.schema_digest),
        approved_keys: Object.freeze(approved), policy_revision: positiveUint64(data.policy_revision) });
}
export function projectFloatContext(context, target) {
    const validated = validateFloatContext(context, target.context_schema);
    const contract = target.persistence_contract;
    if (!contract) return null;
    const allowed = new Set(contract.approved_keys.filter(name => target.context_schema.persist_keys.includes(name)));
    if (Object.entries(target.context_schema.fields).some(([name, field]) => field.required && !allowed.has(name))) return null;
    const projected = Object.fromEntries(Object.entries(validated.context).filter(([name]) => allowed.has(name)));
    return validateFloatContext(projected, target.context_schema).context;
}

// A bounded parser detects decoded duplicate object keys before JSON.parse can
// erase them. Primitive string/number parsing uses the native JSON grammar.
export function parseFloatJSON(source, maximum = 1048576) {
    if (typeof source !== 'string' || utf8Bytes(source) > maximum) invalidFloatWire();
    let position = 0, nodes = 0;
    const whitespace = () => { while (/[\x20\t\r\n]/.test(source[position] || '\0')) position++; };
    function string() {
        const start = position++;
        while (position < source.length) {
            const next = source[position++];
            if (next === '\\') { position++; continue; }
            if (next === '"') {
                const result = JSON.parse(source.slice(start, position));
                text(result, maximum, { empty: true });
                return result;
            }
        }
        return invalidFloatWire();
    }
    function read(depth) {
        if (depth > 20 || ++nodes > 65536) invalidFloatWire();
        whitespace();
        if (source[position] === '"') return string();
        if (source[position] === '{') {
            position++; whitespace();
            const result = Object.create(null), seen = new Set();
            if (source[position] !== '}') while (true) {
                if (source[position] !== '"') invalidFloatWire();
                const name = string();
                if (seen.has(name)) invalidFloatWire();
                seen.add(name); whitespace();
                if (source[position++] !== ':') invalidFloatWire();
                Object.defineProperty(result, name, { enumerable: true, value: read(depth + 1) });
                whitespace();
                if (source[position] !== ',') break;
                position++; whitespace();
            }
            if (source[position++] !== '}') invalidFloatWire();
            return result;
        }
        if (source[position] === '[') {
            position++; whitespace();
            const result = [];
            if (source[position] !== ']') while (true) {
                result.push(read(depth + 1)); whitespace();
                if (source[position] !== ',') break;
                position++;
            }
            if (source[position++] !== ']') invalidFloatWire();
            return result;
        }
        const token = /^(?:null|true|false|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)/.exec(source.slice(position));
        if (!token) invalidFloatWire();
        position += token[0].length;
        const result = JSON.parse(token[0]);
        if (typeof result === 'number' && !Number.isFinite(result)) invalidFloatWire();
        return result;
    }
    const result = read(0); whitespace();
    if (position !== source.length) invalidFloatWire();
    return result;
}

export function typedFloatModuleURL(url, logical, generation) {
    if (url.search || url.hash) return false;
    const pieces = url.pathname.split('/');
    if (pieces.length < 8 || pieces[0] !== '' || pieces[1] !== '_plinth' || pieces[2] !== 'typed' ||
        pieces[3] !== generation || !UUID.test(pieces[3]) || !/^[0-9a-f]{64}$/.test(pieces[4]) ||
        pieces[5] !== logical.panel_id || pieces[6] !== 'client') return false;
    return pieces.slice(7).every(piece => {
        try {
            const decoded = decodeURIComponent(piece);
            return decoded !== '.' && decoded !== '..' && !decoded.includes('/') && !decoded.includes('\\') &&
                !decoded.includes('\0') && isFloatMetadataString(decoded, 1024) &&
                encodeURIComponent(decoded).replace(/[!'()*]/g, char => '%' + char.charCodeAt(0).toString(16).toUpperCase()) === piece;
        } catch { return false; }
    });
}
export function decodeFloatTarget(value, { location = globalThis.location, moduleUrlAllowed = typedFloatModuleURL } = {}) {
    const data = wireRecord(value, ['application_id', 'panel_id', 'type', 'generation', 'version',
        'title', 'icon', 'module_url', 'capability', 'content_types', 'context_schema',
        'logical_install_order', 'persistence_contract']);
    // Only parsed data is serialized; the exact record check above executes no getters.
    // Nested schema/contract records are decoded before the compact target bound.
    const logical = logicalFloat({ application_id: data.application_id, panel_id: data.panel_id });
    if (data.type !== 'float') invalidFloatWire();
    const schema = decodeContextSchema(data.context_schema);
    const types = wireArray(data.content_types, 32).map(item => {
        const canonical = canonicalContentType(item);
        if (canonical !== item) invalidFloatWire();
        return canonical;
    });
    if (new Set(types).size !== types.length) invalidFloatWire();
    match(data.generation, UUID);
    const rawUrl = text(data.module_url, 2048);
    const url = new URL(rawUrl, location.href);
    if (url.origin !== location.origin || url.username || url.password || url.hash ||
        rawUrl !== url.pathname || utf8Bytes(url.href) > 2048 ||
        !moduleUrlAllowed(url, logical, data.generation)) invalidFloatWire();
    const result = Object.freeze({ ...logical, type: 'float', generation: text(data.generation, 256),
        version: text(data.version, 256), title: text(data.title, 1024, { scalars: 256 }),
        icon: match(data.icon, TOKEN), module_url: url.href, capability: capability(data.capability),
        content_types: Object.freeze(types), context_schema: schema,
        logical_install_order: positiveUint64(data.logical_install_order),
        persistence_contract: decodePersistenceContract(data.persistence_contract, schema) });
    if (result.persistence_contract && result.persistence_contract.artifact_digest !== url.pathname.split('/')[4]) invalidFloatWire();
    if (utf8Bytes(JSON.stringify(result)) > 16384) invalidFloatWire();
    return result;
}
function order(left, right) {
    const a = left.logical_install_order, b = right.logical_install_order;
    return a.length - b.length || (a < b ? -1 : a > b ? 1 : 0) ||
        (left.application_id < right.application_id ? -1 : left.application_id > right.application_id ? 1 : 0) ||
        (left.panel_id < right.panel_id ? -1 : left.panel_id > right.panel_id ? 1 : 0);
}
export function decodeFloatCatalog(value, query, options) {
    const hasAdmin = query.include_admin_default === true;
    const data = wireRecord(value, ['schema_version', 'kind', 'targets', 'next_cursor',
        ...(hasAdmin ? ['admin_default_candidate'] : [])]);
    if (data.schema_version !== 1 || data.kind !== 'float') invalidFloatWire();
    const targets = wireArray(data.targets, query.limit || 64).map(item => decodeFloatTarget(item, options));
    const identities = new Set();
    for (let index = 0; index < targets.length; index++) {
        const identity = JSON.stringify([targets[index].application_id, targets[index].panel_id]);
        if ((query.content_type && !targets[index].content_types.includes(query.content_type)) ||
            identities.has(identity) || (index && order(targets[index - 1], targets[index]) >= 0)) invalidFloatWire();
        identities.add(identity);
    }
    const cursor = data.next_cursor === null ? null : text(data.next_cursor, 4096, { nul: false });
    let admin = null;
    if (hasAdmin && data.admin_default_candidate !== null) {
        const candidate = wireRecord(data.admin_default_candidate, ['application_id', 'panel_id', 'generation']);
        admin = Object.freeze({ ...logicalFloat({ application_id: candidate.application_id,
            panel_id: candidate.panel_id }), generation: match(candidate.generation, UUID) });
    }
    return Object.freeze({ targets: Object.freeze(targets), next_cursor: cursor,
        ...(hasAdmin ? { admin_default_candidate: admin } : {}) });
}
export function validateFloatAdmissionRequests(values) {
    const requests = wireArray(values, 38), seen = new Set();
    const result = requests.map(raw => {
        const source = wireRecord(raw);
        const fresh = source.expected_generation === null;
        const data = wireRecord(raw, ['application_id', 'panel_id', 'kind', 'expected_generation',
            fresh ? 'content_type' : 'capability']);
        const logical = logicalFloat({ application_id: data.application_id, panel_id: data.panel_id });
        if (data.kind !== 'float') invalidFloatWire();
        const identity = JSON.stringify([logical.application_id, logical.panel_id]);
        if (seen.has(identity)) invalidFloatWire();
        seen.add(identity);
        return Object.freeze({ ...logical, kind: 'float', expected_generation: fresh ? null : match(data.expected_generation, UUID),
            ...(fresh ? { content_type: canonicalContentType(data.content_type) } : { capability: capability(data.capability) }) });
    });
    if (utf8Bytes(JSON.stringify({ schema_version: 1, targets: result })) > 32768) invalidFloatWire();
    return Object.freeze(result);
}
export const pinnedFloatRequest = target => ({ application_id: target.application_id,
    panel_id: target.panel_id, kind: 'float', expected_generation: target.generation, capability: target.capability });
export function decodeFloatAdmission(value, requests, options) {
    const data = wireRecord(value, ['schema_version', 'results']);
    if (data.schema_version !== 1) invalidFloatWire();
    const results = wireArray(data.results, requests.length), seen = new Set();
    if (results.length !== requests.length) invalidFloatWire();
    const ordered = Array(requests.length);
    for (const raw of results) {
        const source = wireRecord(raw);
        const item = wireRecord(raw, ['index', 'status', ...(source.status === 'available' ? ['target'] : [])]);
        if (!Number.isInteger(item.index) || item.index < 0 || item.index >= requests.length ||
            seen.has(item.index) || !['available', 'unavailable', 'stale'].includes(item.status)) invalidFloatWire();
        seen.add(item.index);
        const request = requests[item.index];
        let target;
        if (item.status === 'available') {
            target = decodeFloatTarget(item.target, options);
            if (target.application_id !== request.application_id || target.panel_id !== request.panel_id ||
                (request.expected_generation === null ? !target.content_types.includes(request.content_type)
                    : target.generation !== request.expected_generation || target.capability !== request.capability)) invalidFloatWire();
        }
        ordered[item.index] = Object.freeze({ index: item.index, status: item.status, ...(target ? { target } : {}) });
    }
    return Object.freeze(ordered);
}
export function decodePersonalFloatDefaults(value) {
    const root = wireRecord(value);
    if (!Reflect.ownKeys(root).length) return Object.freeze([]);
    const data = wireRecord(value, ['value']);
    const defaults = wireRecord(data.value, ['version', 'choices']);
    if (defaults.version !== 1) invalidFloatWire();
    const seen = new Set();
    const choices = wireArray(defaults.choices, 128).map(raw => {
        const item = wireRecord(raw, ['content_type', 'application_id', 'panel_id']);
        const type = canonicalContentType(item.content_type);
        if (type !== item.content_type || seen.has(type)) invalidFloatWire();
        seen.add(type);
        return Object.freeze({ ...logicalFloat({ application_id: item.application_id, panel_id: item.panel_id }), content_type: type });
    });
    if (utf8Bytes(JSON.stringify({ version: 1, choices })) > 32768) invalidFloatWire();
    return Object.freeze(choices);
}
export function decodeFloatLayout(value, options) {
    const source = wireRecord(value);
    const data = wireRecord(value, ['schema_version', 'present', 'policy_revision', 'restore_targets',
        ...(source.present === true ? ['layout'] : [])]);
    if (data.schema_version !== 1 || typeof data.present !== 'boolean') invalidFloatWire();
    const policy = data.policy_revision === null ? null : positiveUint64(data.policy_revision);
    // A malformed stored snapshot is retained as raw bounded data for the
    // non-destructive #43 normalizer, never silently replaced on read.
    const layout = data.present ? copyFloatJSON(data.layout, 32768) : undefined;
    const items = wireArray(data.restore_targets, 5), seen = new Set();
    const results = items.map(raw => {
        const candidate = wireRecord(raw);
        const item = wireRecord(raw, ['index', 'status', ...(candidate.status === 'available' ? ['target'] : [])]);
        if (!Number.isInteger(item.index) || item.index < 0 || item.index >= 5 || seen.has(item.index) ||
            !['available', 'unavailable', 'stale'].includes(item.status)) invalidFloatWire();
        seen.add(item.index);
        return Object.freeze({ index: item.index, status: item.status,
            ...(item.status === 'available' ? { target: decodeFloatTarget(item.target, options) } : {}) });
    });
    let expected = 0;
    if (data.present && layout && typeof layout === 'object' && !Array.isArray(layout) &&
        Object.keys(layout).length === 2 && Object.hasOwn(layout, 'version') &&
        Object.hasOwn(layout, 'floats') && layout.version === 1) {
        const rawFloats = Object.getOwnPropertyDescriptor(layout, 'floats')?.value;
        if (Array.isArray(rawFloats) && rawFloats.length <= 5) expected = rawFloats.length;
    }
    if (results.length !== expected || results.some(item => item.index >= expected) ||
        (policy === null && results.some(item => item.status === 'available'))) invalidFloatWire();
    return Object.freeze({ present: data.present, layout,
        policy_revision: policy, restore_targets: Object.freeze(results) });
}
export function encodeFloatLayout(snapshot, bindings) {
    const layout = validateFloatSnapshot(snapshot).value;
    const rows = wireArray(bindings, 5);
    if (rows.length !== layout.floats.length) invalidFloatWire();
    const seen = new Set();
    const decoded = rows.map(raw => {
        const item = wireRecord(raw, ['index', 'generation', 'artifact_digest', 'schema_digest',
            'projection_contract', 'policy_revision']);
        if (!Number.isInteger(item.index) || item.index < 0 || item.index >= rows.length || seen.has(item.index)) invalidFloatWire();
        seen.add(item.index);
        const contract = wireRecord(item.projection_contract, ['id', 'version']);
        if (!Number.isSafeInteger(contract.version) || contract.version < 1) invalidFloatWire();
        return { index: item.index, generation: match(item.generation, UUID),
            artifact_digest: digest(item.artifact_digest), schema_digest: digest(item.schema_digest),
            projection_contract: { id: match(contract.id, PANEL), version: contract.version },
            policy_revision: positiveUint64(item.policy_revision) };
    });
    const body = JSON.stringify({ schema_version: 1, layout, bindings: decoded });
    if (utf8Bytes(body) > 65536) invalidFloatWire();
    return body;
}
