import { groupPolicy, isUuid, validateGroupInput } from './api.js';

const clone = value => JSON.parse(JSON.stringify(value));
const blank = () => ({ name: '', description: '' });
const failure = error => ({ code: error.code || error.name || 'error',
    message: error.message || 'Groups request failed', status: error.status ?? null });
const unverified = () => ({ code: 'session-unverified',
    message: 'Current session could not be verified; retry the read.', status: null });

export function createGroupsController({ api, publish = () => {}, onSessionEnd = () => {},
    AbortController: Abort = globalThis.AbortController }) {
    let destroyed = false;
    let owner = null;
    let nextRequest = 0;
    const initial = phase => ({ phase, actor: null, items: [], selectedId: null, detail: null,
        createDraft: blank(), editDraft: blank(), memberId: '', deleteConfirmation: '', formError: null,
        pending: { session: false, list: false, detail: false },
        readErrors: { session: null, list: null, detail: null }, attempts: [] });
    let state = initial('prepared');
    const owns = captured => !destroyed && owner === captured && captured?.active;
    const snapshot = () => clone({ ...state, policy: groupPolicy(state.detail) });
    const emit = () => publish(snapshot());
    function retire(phase) {
        const old = owner;
        if (old) old.active = false; // Close admission before synchronous abort callbacks.
        owner = null;
        state = initial(phase);
        if (old) for (const read of old.reads.values()) read.abort.abort();
        emit();
    }
    function sessionEnded() { if (!destroyed) retire('session-ended'); }
    function authError(captured, error) {
        if (!owns(captured) || error?.status !== 401) return false;
        sessionEnded();
        onSessionEnd(error.code || 'not_authenticated');
        return true;
    }
    async function actorMatches(captured, options) {
        const session = await api.session(options);
        if (!options.isCurrent()) return false;
        if (session.user.id === captured.actor.id && session.session.id === captured.actor.sessionId) return true;
        sessionEnded();
        onSessionEnd('session_rotated');
        return false;
    }
    async function probe(captured, options) {
        try { return await actorMatches(captured, options); }
        catch (error) {
            if (!options.isCurrent() || authError(captured, error)) return false;
            return null;
        }
    }
    function clearDeniedData(captured, except) {
        state.items = [];
        state.selectedId = null;
        state.detail = null;
        state.editDraft = blank();
        state.memberId = '';
        state.deleteConfirmation = '';
        captured.selection++;
        captured.editRevision++;
        for (const attempt of state.attempts) {
            attempt.response = null;
            attempt.observed = null;
            attempt.reconciliation = 'none';
            attempt.reconciliationError = null;
        }
        for (const [key, entry] of [...captured.reads]) {
            if (!(key.startsWith('reconcile:') || ['list', 'detail'].includes(key) && key !== except)) continue;
            captured.reads.delete(key);
            entry.abort.abort();
            if (['list', 'detail'].includes(key)) state.pending[key] = false;
        }
    }
    async function readRequest(captured, key, request, accept) {
        if (!owns(captured)) return null;
        const old = captured.reads.get(key);
        const entry = { abort: new Abort() };
        captured.reads.set(key, entry);
        old?.abort.abort();
        const current = () => owns(captured) && captured.reads.get(key) === entry;
        const options = { signal: entry.abort.signal, isCurrent: current };
        state.pending[key] = true;
        state.readErrors[key] = null;
        emit();
        let stage = key === 'session' ? 'request' : 'preflight';
        try {
            if (!current()) return null;
            if (key !== 'session' && !await actorMatches(captured, options)) return null;
            if (!current()) return null;
            stage = 'request';
            const result = await request(options);
            if (!current()) return null;
            stage = 'postflight';
            if (key !== 'session' && !await actorMatches(captured, options)) return null;
            if (!current()) return null;
            stage = 'accept';
            accept(result);
            return result;
        } catch (error) {
            if (!current() || authError(captured, error)) return null;
            if (key !== 'session') {
                if (stage !== 'request') error = unverified();
                else {
                    const match = await probe(captured, options);
                    if (!current() || match === false) return null;
                    if (match === null) error = unverified();
                }
            }
            if (error.status === 403) clearDeniedData(captured, key);
            state.readErrors[key] = failure(error);
            return null;
        } finally {
            if (current()) {
                captured.reads.delete(key);
                state.pending[key] = false;
                emit();
            }
        }
    }
    async function activate() {
        if (destroyed || owner?.active && (owner.actor || state.pending.session)) return;
        if (owner?.active) retire('inactive');
        const captured = { active: true, actor: null, reads: new Map(), tail: Promise.resolve(),
            selection: 0, editRevision: 0 };
        owner = captured;
        state = initial('loading');
        emit();
        const result = await readRequest(captured, 'session', options => api.session(options), session => {
            captured.actor = Object.freeze({ id: session.user.id, username: session.user.username,
                sessionId: session.session.id });
            state.actor = captured.actor;
            state.phase = 'active';
        });
        if (result && owns(captured)) await loadList();
    }
    function loadList() {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        return readRequest(captured, 'list', options => api.list(options), result => { state.items = result.groups; });
    }
    function loadDetail(captured, id, fillDraft) {
        const revision = captured.editRevision;
        return readRequest(captured, 'detail', options => api.detail(id, options), result => {
            state.detail = result;
            if (fillDraft && revision === captured.editRevision) {
                state.editDraft = { name: result.name, description: result.description ?? '' };
            }
        });
    }
    function select(id) {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        if (!isUuid(id)) {
            state.formError = { code: 'validation', message: 'Group id must be a UUID', status: null };
            emit();
            return Promise.resolve(null);
        }
        id = id.toLowerCase();
        captured.selection++;
        captured.editRevision++;
        state.selectedId = id;
        state.detail = null;
        state.editDraft = blank();
        state.memberId = '';
        state.deleteConfirmation = '';
        state.formError = null;
        return loadDetail(captured, id, true);
    }
    function setDraft(key, patch) {
        if (!owns(owner) || !owner.actor) return false;
        if (key === 'editDraft' && !groupPolicy(state.detail).editMetadata) return false;
        if (!patch || typeof patch !== 'object' || Array.isArray(patch) ||
            Object.keys(patch).some(name => !['name', 'description'].includes(name) || typeof patch[name] !== 'string')) {
            throw new TypeError('Draft fields must be name/description strings');
        }
        state[key] = { ...state[key], ...patch };
        if (key === 'editDraft') owner.editRevision++;
        state.formError = null;
        emit();
        return true;
    }
    function setText(key, value) {
        if (!owns(owner) || !owner.actor) return false;
        if (typeof value !== 'string') throw new TypeError('Form value must be a string');
        state[key] = value;
        state.formError = null;
        emit();
        return true;
    }
    function updateAttempt(captured, requestId, patch) {
        if (!owns(captured)) return;
        const attempt = state.attempts.find(item => item.requestId === requestId);
        if (attempt) { Object.assign(attempt, patch); emit(); }
    }
    async function actorRead(captured, key) {
        if (!owns(captured)) return false;
        const entry = { abort: new Abort() };
        captured.reads.set(key, entry);
        const options = { signal: entry.abort.signal,
            isCurrent: () => owns(captured) && captured.reads.get(key) === entry };
        try { return await probe(captured, options); }
        finally { if (captured.reads.get(key) === entry) captured.reads.delete(key); }
    }
    async function reconcile(requestId) {
        const captured = owner;
        const attempt = state.attempts.find(item => item.requestId === requestId);
        if (!owns(captured) || !attempt || attempt.status !== 'unknown') return null;
        const key = 'reconcile:' + requestId;
        const old = captured.reads.get(key);
        const entry = { abort: new Abort() };
        captured.reads.set(key, entry);
        old?.abort.abort();
        const current = () => owns(captured) && captured.reads.get(key) === entry;
        const options = { signal: entry.abort.signal, isCurrent: current };
        const observed = { list: null, detail: null };
        updateAttempt(captured, requestId, { reconciliation: 'pending', reconciliationError: null });
        let stage = 'preflight';
        try {
            if (!await actorMatches(captured, options) || !current()) return null;
            stage = 'request';
            const list = await api.list(options);
            stage = 'postflight';
            if (!current() || !await actorMatches(captured, options) || !current()) return null;
            observed.list = list;
            if (attempt.target.id) {
                stage = 'preflight';
                if (!await actorMatches(captured, options) || !current()) return null;
                stage = 'request';
                try {
                    const detail = await api.detail(attempt.target.id, options);
                    stage = 'postflight';
                    if (!current() || !await actorMatches(captured, options) || !current()) return null;
                    observed.detail = detail;
                } catch (error) {
                    if (!current() || authError(captured, error)) return null;
                    if (stage !== 'request' || error.status !== 404) throw error;
                    stage = 'postflight';
                    if (!await actorMatches(captured, options) || !current()) return null;
                    observed.detail = { absent: true };
                }
            }
            // Observation never changes the UNKNOWN mutation outcome or proves membership.
            updateAttempt(captured, requestId, { reconciliation: 'complete', observed });
            return observed;
        } catch (error) {
            if (!current() || authError(captured, error)) return null;
            if (stage !== 'request') error = unverified();
            else {
                const match = await probe(captured, options);
                if (!current() || match === false) return null;
                if (match === null) error = unverified();
            }
            if (error.status === 403) {
                observed.list = null; observed.detail = null;
                clearDeniedData(captured, key);
            }
            updateAttempt(captured, requestId, { reconciliation: 'failed', observed,
                reconciliationError: failure(error) });
            return null;
        } finally { if (current()) captured.reads.delete(key); }
    }
    function mutation(operation, target, invoke) {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        const requestId = ++nextRequest;
        const intent = clone(target);
        const selection = captured.selection;
        state.attempts.push({ requestId, actor: clone(captured.actor), operation, target: intent,
            status: 'queued', response: null, error: null, reconciliation: 'none', observed: null });
        emit();
        const task = async () => {
            if (!owns(captured)) return null;
            updateAttempt(captured, requestId, { status: 'pending' });
            let dispatched = false;
            try {
                const admitted = await actorRead(captured, 'admission:' + requestId);
                if (!owns(captured) || admitted === false) return null;
                if (admitted === null) {
                    updateAttempt(captured, requestId, { status: 'failed', error: unverified() });
                    return null;
                }
                dispatched = true;
                const response = await invoke({ isCurrent: () => owns(captured) });
                if (!owns(captured)) return null;
                const attributed = await actorRead(captured, 'ack:' + requestId);
                if (!owns(captured) || attributed === false) return null;
                if (attributed === null) {
                    updateAttempt(captured, requestId, { status: 'unknown', error: unverified() });
                    return null;
                }
                updateAttempt(captured, requestId, { status: 'succeeded', response });
                if (operation === 'delete' && state.selectedId === intent.id) {
                    // Retire a pre-delete GET before it can resurrect the acknowledged row.
                    const previous = captured.reads.get('detail');
                    captured.reads.delete('detail');
                    captured.editRevision++;
                    captured.selection++;
                    state.selectedId = null; state.detail = null; state.editDraft = blank();
                    state.memberId = ''; state.deleteConfirmation = '';
                    state.pending.detail = false;
                    state.readErrors.detail = null;
                    previous?.abort.abort();
                }
                await loadList();
                if (!owns(captured)) return response;
                if (operation === 'create' && captured.selection === selection) await select(response.id);
                else if (state.selectedId === intent.id) await loadDetail(captured, intent.id, false);
                return response;
            } catch (error) {
                if (!owns(captured) || authError(captured, error)) return null;
                const match = dispatched && error.dispatched !== false ?
                    await actorRead(captured, 'failure:' + requestId) : true;
                if (!owns(captured) || match === false) return null;
                if (match === null) {
                    updateAttempt(captured, requestId, { status: 'unknown', error: unverified() });
                } else {
                    if (error.status === 403) clearDeniedData(captured);
                    updateAttempt(captured, requestId, { status: error.unknown ? 'unknown' : 'failed',
                        error: failure(error) });
                }
                return null;
            }
        };
        const result = captured.tail.then(task, task);
        captured.tail = result.catch(() => {});
        return result;
    }
    function submit(prepare) {
        if (!owns(owner) || !owner.actor) return Promise.resolve(null);
        try { state.formError = null; return prepare(); }
        catch (error) {
            state.formError = { code: 'validation', message: error.message, status: null };
            emit();
            return Promise.resolve(null);
        }
    }
    function selected(policy) {
        const detail = state.detail;
        if (!detail || !groupPolicy(detail)[policy]) throw new TypeError(
            policy === 'editMembers' ? 'Everyone membership is virtual and cannot be changed' :
                'Built-in group metadata cannot be changed; select an editable group');
        return detail;
    }
    const memberAction = adding => submit(() => {
        const detail = selected('editMembers'), id = detail.id;
        if (!isUuid(state.memberId)) throw new TypeError('User id must be a UUID');
        const userId = state.memberId.toLowerCase();
        return mutation(adding ? 'add-member' : 'remove-member', { id, userId },
            options => adding ? api.addMember(id, userId, options) : api.removeMember(id, userId, options));
    });
    return {
        activate, loadList, select, reconcile, sessionEnded, read: snapshot,
        deactivate() { if (!destroyed) retire('inactive'); },
        destroy() { if (!destroyed) { destroyed = true; retire('destroyed'); } },
        setCreateDraft: patch => setDraft('createDraft', patch),
        setEditDraft: patch => setDraft('editDraft', patch),
        setMemberId: value => setText('memberId', value),
        setDeleteConfirmation: value => setText('deleteConfirmation', value),
        create: () => submit(() => {
            const fields = validateGroupInput(state.createDraft);
            return mutation('create', fields, options => api.create(fields, options));
        }),
        update: () => submit(() => {
            const id = selected('editMetadata').id, fields = validateGroupInput(state.editDraft);
            return mutation('update', { id, ...fields }, options => api.update(id, fields, options));
        }),
        remove: () => submit(() => {
            const detail = selected('deleteGroup'), id = detail.id, name = detail.name;
            if (state.deleteConfirmation !== name) throw new TypeError('Type the selected group name to confirm deletion');
            return mutation('delete', { id, name }, options => api.remove(id, options));
        }),
        addMember: () => memberAction(true), removeMember: () => memberAction(false),
    };
}
