import { withCsrf } from '@plinth/frontend/sdk';

export class OwnerRetiredError extends Error {
    constructor() { super('Groups request owner retired'); this.name = 'OwnerRetiredError'; }
}

export class GroupsApiError extends Error {
    constructor(message, { status = null, code = 'transport-error', unknown = false,
                           dispatched = false } = {}) {
        super(message);
        this.name = 'GroupsApiError';
        Object.assign(this, { status, code, unknown, dispatched });
    }
}

const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const text = value => typeof value === 'string' && value.length > 0;
export const isUuid = value => typeof value === 'string' &&
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(value);
const nameValid = value => typeof value === 'string' && /^[a-z0-9_-]{1,64}$/.test(value);
const descriptionValid = value => typeof value === 'string' && new TextEncoder().encode(value).length <= 256;

export function validateGroupInput(input) {
    if (!object(input) || !nameValid(input.name)) {
        throw new TypeError('Group name must be 1–64 lowercase ASCII letters, digits, hyphens or underscores');
    }
    const description = input.description === undefined ? '' : input.description;
    if (!descriptionValid(description)) throw new TypeError('Description must be a string of at most 256 UTF-8 bytes');
    return { name: input.name, description };
}

export function groupPolicy(group) {
    return {
        editMetadata: !!group && group.built_in === false,
        deleteGroup: !!group && group.built_in === false,
        editMembers: !!group && group.name !== 'everyone',
        virtualMembership: !!group && group.name === 'everyone',
    };
}

const record = value => object(value) && isUuid(value.id) && nameValid(value.name) &&
    typeof value.built_in === 'boolean' && text(value.created_at) &&
    (!Object.hasOwn(value, 'description') || descriptionValid(value.description));
const groupValue = value => ({ id: value.id, name: value.name,
    ...(Object.hasOwn(value, 'description') ? { description: value.description } : {}),
    built_in: value.built_in, created_at: value.created_at });
const knownRejections = new Set([400, 401, 403, 404, 409]);

export function createGroupsApi({ fetch: fetchRequest = globalThis.fetch, csrf = withCsrf } = {}) {
    const current = options => {
        if (options.isCurrent && !options.isCurrent()) throw new OwnerRetiredError();
    };
    const groupPath = id => {
        if (!isUuid(id)) throw new TypeError('Group id must be a UUID');
        return '/api/groups/' + encodeURIComponent(id.toLowerCase());
    };
    const memberId = id => {
        if (!isUuid(id)) throw new TypeError('User id must be a UUID');
        return id.toLowerCase();
    };
    async function request(url, init, options, expected, valid) {
        const mutation = init.method !== 'GET';
        current(options);
        let prepared;
        try {
            prepared = csrf(url, { ...init, credentials: 'same-origin',
                ...(mutation ? {} : { cache: 'no-store' }),
                ...(options.signal ? { signal: options.signal } : {}) });
        } catch (error) {
            current(options);
            throw new GroupsApiError('Could not prepare groups request', { code: 'request-preparation' });
        }
        current(options);
        let response;
        try { response = await fetchRequest(url, prepared); }
        catch (error) {
            current(options);
            throw new GroupsApiError('Groups request did not return a response', {
                unknown: mutation, dispatched: true,
            });
        }
        current(options);
        let body;
        try { body = await response.json(); }
        catch (error) {
            current(options);
            throw new GroupsApiError('Groups response is not JSON', {
                status: response.status, code: 'invalid-response', unknown: mutation, dispatched: true,
            });
        }
        current(options);
        if (response.status !== expected) {
            const envelope = object(body) && object(body.error) ? body.error : null;
            const code = envelope && text(envelope.code) ? envelope.code :
                object(body) && text(body.error) ? body.error : 'http-error';
            const structured = code !== 'http-error';
            const message = envelope && text(envelope.message) ? envelope.message :
                object(body) && text(body.message) ? body.message : `Groups endpoint returned HTTP ${response.status}`;
            throw new GroupsApiError(message, { status: response.status, code, dispatched: true,
                unknown: mutation && !(structured && knownRejections.has(response.status)) });
        }
        if (!valid(body)) throw new GroupsApiError('Groups response has an invalid shape', {
            status: response.status, code: 'invalid-response', unknown: mutation, dispatched: true,
        });
        return body;
    }
    const json = (method, body) => ({ method, headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body) });
    return {
        async session(options = {}) {
            const body = await request('/api/auth/session', { method: 'GET' }, options, 200,
                value => object(value) && object(value.user) && isUuid(value.user.id) && text(value.user.username) &&
                    object(value.session) && isUuid(value.session.id));
            return { user: { id: body.user.id, username: body.user.username }, session: { id: body.session.id } };
        },
        async list(options = {}) {
            const body = await request('/api/groups', { method: 'GET' }, options, 200,
                value => object(value) && Array.isArray(value.groups) && value.groups.every(record) &&
                    new Set(value.groups.map(group => group.id)).size === value.groups.length);
            return { groups: body.groups.map(groupValue) };
        },
        async detail(id, options = {}) {
            id = memberId(id);
            const body = await request(groupPath(id), { method: 'GET' }, options, 200,
                value => record(value) && value.id === id && Number.isSafeInteger(value.member_count) &&
                    value.member_count >= 0);
            return { ...groupValue(body), member_count: body.member_count };
        },
        async create(input, options = {}) {
            const fields = validateGroupInput(input);
            const body = await request('/api/groups', json('POST', fields), options, 201,
                value => record(value) && value.name === fields.name && (value.description ?? '') === fields.description);
            return groupValue(body);
        },
        async update(id, input, options = {}) {
            id = memberId(id);
            const fields = validateGroupInput(input);
            const body = await request(groupPath(id), json('PUT', fields), options, 200,
                value => record(value) && value.id === id && value.name === fields.name &&
                    (value.description ?? '') === fields.description);
            return groupValue(body);
        },
        async remove(id, options = {}) {
            await request(groupPath(id), { method: 'DELETE' }, options, 200,
                value => object(value) && value.status === 'deleted');
            return { status: 'deleted' };
        },
        async addMember(id, userId, options = {}) {
            id = memberId(id);
            const target = memberId(userId);
            const body = await request(groupPath(id) + '/members', json('POST', { user_id: target }), options, 201,
                value => object(value) && value.group_id === id && value.user_id === target && text(value.added_at));
            return { group_id: body.group_id, user_id: body.user_id, added_at: body.added_at };
        },
        async removeMember(id, userId, options = {}) {
            await request(groupPath(id) + '/members/' + encodeURIComponent(memberId(userId)),
                { method: 'DELETE' }, options, 200, value => object(value) && value.status === 'removed');
            return { status: 'removed' };
        },
    };
}
