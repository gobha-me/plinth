import { withCsrf } from '@plinth/frontend/sdk';

export class OwnerRetiredError extends Error {
    constructor() { super('Package request owner retired'); this.name = 'OwnerRetiredError'; }
}

export class PackageApiError extends Error {
    constructor(message, { status = null, code = 'transport-error', body = null,
                           unknown = false, dispatched = false } = {}) {
        super(message);
        this.name = 'PackageApiError';
        Object.assign(this, { status, code, body, unknown, dispatched });
    }
}

const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const text = value => typeof value === 'string' && value.length > 0;
const record = value => object(value) && ['id', 'name', 'version', 'state'].every(key => text(value[key]));
const uuid = value => typeof value === 'string' &&
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(value);

export function createPackagesApi({ fetch: fetchRequest = globalThis.fetch,
                                    csrf = withCsrf,
                                    FormData: Form = globalThis.FormData } = {}) {
    const current = options => {
        if (options.isCurrent && !options.isCurrent()) throw new OwnerRetiredError();
    };
    const path = id => {
        if (!uuid(id)) throw new TypeError('Package id must be a UUID');
        return '/api/packages/' + encodeURIComponent(id);
    };
    async function request(url, init, options, expected, valid) {
        const mutation = !['GET', 'HEAD'].includes(init.method);
        current(options);
        let prepared;
        try {
            prepared = csrf(url, {
                ...init, credentials: 'same-origin',
                ...(init.method === 'GET' ? { cache: 'no-store' } : {}),
                ...(options.signal ? { signal: options.signal } : {}),
            });
        } catch (error) {
            current(options);
            throw new PackageApiError('Could not prepare package request', {
                code: 'request-preparation', dispatched: false,
            });
        }
        current(options);
        let response;
        try {
            response = await fetchRequest(url, prepared);
        } catch (error) {
            current(options);
            throw new PackageApiError('Package request did not return a response', {
                unknown: mutation, dispatched: true, code: 'transport-error',
            });
        }
        current(options);
        if (expected === 204 && response.status === 204) return null;
        let body;
        try { body = await response.json(); }
        catch (error) {
            current(options);
            throw new PackageApiError('Package response is not JSON', {
                status: response.status, unknown: mutation, dispatched: true, code: 'invalid-response',
            });
        }
        current(options);
        if (response.status !== expected) {
            const envelope = object(body) && object(body.error) ? body.error : null;
            const code = envelope?.code || (object(body) && text(body.kind) ? body.kind :
                (object(body) && text(body.error) ? body.error : 'http-error'));
            const message = envelope?.message || (object(body) && text(body.message) ? body.message :
                `Package endpoint returned HTTP ${response.status}`);
            const structured = (envelope && text(envelope.code) && text(envelope.message)) ||
                (object(body) && text(body.kind) && text(body.message)) ||
                (object(body) && text(body.error));
            // A structured server error is not necessarily a rolled-back
            // mutation. Install can commit ACTIVE and then fail lock release;
            // other 5xx transitions can also leave a partial/unknown state.
            const possibleCommit = response.status >= 500 || (object(body) &&
                (body.state === 'ACTIVE' || body.failed_at_stage === 'ACTIVE'));
            throw new PackageApiError(message, { status: response.status, code, body,
                unknown: mutation && (!structured || response.status < 400 || possibleCommit),
                dispatched: true });
        }
        if (!valid(body)) throw new PackageApiError('Package response has an invalid shape', {
            status: response.status, unknown: mutation, dispatched: true, code: 'invalid-response',
        });
        return body;
    }
    const pagination = ({ limit = 20, offset = 0, includeFailed = false } = {}) => {
        if (!Number.isInteger(limit) || limit < 1 || limit > 200 ||
            !Number.isInteger(offset) || offset < 0 || offset > 2147483647 ||
            typeof includeFailed !== 'boolean') throw new TypeError('Invalid package page');
        return `?limit=${limit}&offset=${offset}` + (includeFailed ? '&include_failed=1' : '');
    };
    return {
        session: (options = {}) => request('/api/auth/session', { method: 'GET' }, options, 200,
            body => object(body) && object(body.user) && text(body.user.id) && text(body.user.username) &&
                object(body.session) && text(body.session.id)),
        list: (page = {}, options = {}) => request('/api/packages' + pagination(page),
            { method: 'GET' }, options, 200, body => object(body) && Array.isArray(body.items) &&
                body.items.every(record) && Number.isInteger(body.limit) && body.limit >= 1 &&
                body.limit <= 200 && Number.isInteger(body.offset) && body.offset >= 0),
        detail: (id, options = {}) => request(path(id), { method: 'GET' }, options, 200, record),
        upload(file, { dryRun = false } = {}, options = {}) {
            if (!file || typeof file.name !== 'string' || !/\.zip$/i.test(file.name) ||
                typeof file.size !== 'number' || file.size <= 0) throw new TypeError('Select a nonempty ZIP file');
            if (typeof dryRun !== 'boolean') throw new TypeError('Invalid dry-run option');
            const body = new Form();
            body.append('package', file.blob || file, file.name);
            return request('/api/packages' + (dryRun ? '?dry_run=1' : ''),
                { method: 'POST', body }, options, dryRun ? 200 : 201,
                dryRun ? value => object(value) && value.state === 'VALIDATING' && text(value.name) &&
                    text(value.version) && object(value.validation_report) : record);
        },
        transition(id, action, options = {}) {
            if (!['enable', 'disable'].includes(action)) throw new TypeError('Invalid package action');
            return request(path(id), { method: 'PATCH', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ action }) }, options, 200,
                value => record(value) && value.id === id && value.action === action);
        },
        uninstall: (id, options = {}) => request(path(id) + '?confirm=true',
            { method: 'DELETE' }, options, 204, () => false),
    };
}
