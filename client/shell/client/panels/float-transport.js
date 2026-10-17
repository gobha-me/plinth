import { withCsrf } from '@plinth/frontend/sdk';
import { canonicalContentType, decodeFloatAdmission, decodeFloatCatalog, decodeFloatLayout,
    decodePersonalFloatDefaults, encodeFloatLayout, parseFloatJSON, utf8Bytes,
    validateFloatAdmissionRequests, wireRecord, invalidFloatWire } from './float-wire.js';

export class FloatRequestError extends Error {
    constructor(code = 'authority-unavailable', { status = null, uncertain = false } = {}) {
        super('Float request could not be completed');
        this.name = 'FloatRequestError'; this.code = code;
        this.status = status; this.uncertain = uncertain;
    }
}
export function readFloatPlatformProfile(document = globalThis.document) {
    const elements = document.querySelectorAll('meta[name="plinth-platform-profile"]');
    if (elements.length !== 1) throw new FloatRequestError('reload-required');
    const profile = elements[0].getAttribute('content');
    if (typeof profile !== 'string' ||
        !/^v1:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}:[0-9a-f]{64}$/.test(profile)) {
        throw new FloatRequestError('reload-required');
    }
    return profile;
}

// The Promise does not settle until fetch AND its body/cancellation settle.
// Abort/deadline callers must retain their physical lane until that Promise.
export async function readBoundedFloatResponse(response, maximum) {
    const advertised = response.headers.get('Content-Length');
    const reader = response.body?.getReader();
    if (!reader) throw new FloatRequestError('invalid-response');
    const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
    let length = 0, output = '';
    try {
        if (advertised !== null && (!/^[0-9]+$/.test(advertised) || Number(advertised) > maximum)) {
            throw new FloatRequestError('invalid-response');
        }
        while (true) {
            const item = await reader.read();
            if (item.done) break;
            length += item.value.byteLength;
            if (length > maximum) throw new FloatRequestError('invalid-response');
            output += decoder.decode(item.value, { stream: true });
        }
        output += decoder.decode();
        return parseFloatJSON(output, maximum);
    } catch {
        try { await reader.cancel(); } catch { /* Cancellation failure does not imply server rollback. */ }
        throw new FloatRequestError('invalid-response');
    } finally { reader.releaseLock(); }
}

export function createFloatTransport({ fetch: fetchRequest = globalThis.fetch, csrf = withCsrf,
    platformProfile, location = globalThis.location, moduleUrlAllowed } = {}) {
    if (typeof platformProfile !== 'string' || !platformProfile.length || platformProfile.length > 512) {
        throw new TypeError('missing trusted platform profile');
    }
    const decoderOptions = { location, ...(moduleUrlAllowed ? { moduleUrlAllowed } : {}) };
    async function request(path, { method = 'GET', body, signal, maximum = 1048576, profile = true } = {}) {
        const mutation = method !== 'GET';
        if (signal?.aborted) throw new FloatRequestError('cancelled');
        let response;
        try {
            response = await fetchRequest(path, csrf(path, { method, credentials: 'same-origin',
                cache: 'no-store', redirect: 'error', signal,
                headers: { Accept: 'application/json', ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
                ...(body === undefined ? {} : { body }) }));
        } catch { throw new FloatRequestError(signal?.aborted ? 'cancelled' : 'authority-unavailable', { uncertain: mutation }); }
        // Read even error bodies within the same finite budget. Never render raw server data.
        let value, invalidBody = false;
        try { value = await readBoundedFloatResponse(response, maximum); }
        catch { invalidBody = true; }
        // Terminal identity/profile evidence survives logical cancellation, but
        // only AFTER body consumption/cancellation physically settles. The
        // document request owner still fences dispatch from retired frames.
        if (response.status === 401) throw new FloatRequestError('session-ended',
            { status: response.status, uncertain: mutation });
        if (response.status === 200 && profile && response.headers.get('X-Plinth-Platform-Profile') !== platformProfile) {
            throw new FloatRequestError('reload-required', { uncertain: mutation });
        }
        if (invalidBody) throw new FloatRequestError('invalid-response',
            { status: response.status, uncertain: mutation });
        if (signal?.aborted) throw new FloatRequestError('cancelled', { uncertain: mutation });
        if (response.status !== 200) throw new FloatRequestError('authority-unavailable',
            { status: response.status, uncertain: mutation && response.status >= 500 });
        return value;
    }
    const decode = (operation, transform, uncertain = false) => operation.then(value => {
        try { return transform(value); } catch { throw new FloatRequestError('invalid-response', { uncertain }); }
    });
    return Object.freeze({
        catalog(query = {}, { signal } = {}) {
            const { content_type, cursor = null, limit = 64, include_admin_default = false } = query;
            if (!Number.isInteger(limit) || limit < 1 || limit > 64 || typeof include_admin_default !== 'boolean' ||
                (include_admin_default && content_type === undefined)) invalidFloatWire();
            const type = content_type === undefined ? undefined : canonicalContentType(content_type);
            if (cursor !== null && (typeof cursor !== 'string' || !cursor.length || utf8Bytes(cursor) > 4096)) invalidFloatWire();
            const params = new URLSearchParams({ kind: 'float', limit: String(limit) });
            if (type !== undefined) params.set('content_type', type);
            if (cursor !== null) params.set('cursor', cursor);
            if (include_admin_default) params.set('include_admin_default', '1');
            const normalized = { content_type: type, cursor, limit, include_admin_default };
            return decode(request('/api/frontend/surfaces?' + params, { signal }),
                value => decodeFloatCatalog(value, normalized, decoderOptions));
        },
        admit(targets, { signal } = {}) {
            const validated = validateFloatAdmissionRequests(targets);
            return decode(request('/api/frontend/surfaces/admission', { method: 'POST', signal,
                body: JSON.stringify({ schema_version: 1, targets: validated }) }),
            value => decodeFloatAdmission(value, validated, decoderOptions));
        },
        personalDefaults({ signal } = {}) {
            return decode(request('/api/cap/shell.preferences.get', { method: 'POST', signal, profile: false,
                body: '{"args":{"key":"shell.handler_defaults"}}', maximum: 65536 }), value => {
                // Existing api_cap.cpp make_success envelope, not the new
                // surfaces protocol. Keep its resolver metadata intact.
                const envelope = wireRecord(value, ['ok', 'value', 'resolved_tier', 'provider_type']);
                if (envelope.ok !== true || !['tier1', 'tier2', 'tier3'].includes(envelope.resolved_tier) ||
                    !['kernel', 'extension', 'sidecar'].includes(envelope.provider_type)) invalidFloatWire();
                return decodePersonalFloatDefaults(envelope.value);
            });
        },
        readLayout({ signal } = {}) {
            return decode(request('/api/frontend/float-layout', { signal, maximum: 131072 }),
                value => decodeFloatLayout(value, decoderOptions));
        },
        commitLayout(layout, bindings, { signal } = {}) {
            const body = encodeFloatLayout(layout, bindings);
            return decode(request('/api/frontend/float-layout', { method: 'PUT', body, signal, maximum: 2048 }), value => {
                const data = wireRecord(value, ['schema_version', 'saved']);
                if (data.schema_version !== 1 || data.saved !== true) invalidFloatWire();
                return Object.freeze({ saved: true });
            }, true);
        },
    });
}
