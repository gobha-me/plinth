// Test-fixture scheduling only; never grants authority or changes application
// timers. Observe actual requests, and suspend one real Chromium background tab.
const failure = () => new Error('installed float read-lane coordination failed');

export async function boundedStep(run, deadline) {
    const timeout = Math.ceil(deadline - performance.now());
    if (!Number.isFinite(timeout) || timeout <= 0) throw failure();
    let timer;
    try {
        return await Promise.race([run(), new Promise((_, reject) => {
            timer = setTimeout(() => reject(failure()), timeout);
        })]);
    } finally { clearTimeout(timer); }
}

export function authorityRecoveryDOM(timeout) {
    return new Promise((resolve, reject) => {
        if (!Number.isFinite(timeout) || timeout <= 0) { reject(new Error('authority recovery deadline')); return; }
        let observer, timer, done = false;
        const finish = ready => {
            if (done) return;
            done = true; clearTimeout(timer); observer?.disconnect();
            if (ready) resolve(true); else reject(new Error('authority recovery deadline'));
        };
        const check = () => {
            try {
                const main = document.querySelector('.launcher-main');
                const home = document.querySelector('.launcher-home');
                const host = document.querySelector('.panel-host');
                const callerTile = document.querySelector('.launcher-grid .launcher-tile[aria-label="Float Caller"]');
                if (main?.getAttribute('aria-busy') === 'false' && home && !home.hidden &&
                    host?.isConnected && host.childElementCount === 0 &&
                    callerTile?.isConnected && home.contains(callerTile) &&
                    !document.querySelector('#installed-float-caller')) finish(true);
            } catch { finish(false); }
        };
        try {
            observer = new MutationObserver(check);
            observer.observe(document.documentElement, { subtree: true, childList: true,
                attributes: true, attributeFilter: ['hidden', 'aria-busy'] });
            timer = setTimeout(() => finish(false), timeout);
            check();
        } catch { finish(false); }
    });
}

/** Actual native recovery observation only; never sends a request or hint. */
export function observeAuthorityRecovery(page, origin) {
    const sockets = new Map(), tasks = new Set();
    let closed = false, active = null, overflow = false, arms = 0;
    const channel = 'plinth:system:applications.changed';
    const finish = (entry, rejected = null) => {
        if (active !== entry || entry.done) return;
        entry.done = true; clearTimeout(entry.timer); active = null;
        if (rejected) entry.reject(rejected.error);
        else entry.resolve(Object.freeze({ terminal: 1, new_socket_ack: 1, fresh_catalog: 1, home_empty: 1 }));
    };
    const fail = entry => finish(entry, { error: failure() });
    const reject = (entry, error) => finish(entry, { error });
    const decode = event => {
        const payload = event.payload;
        if (typeof payload !== 'string' || payload.length > 2048 || Buffer.byteLength(payload, 'utf8') > 2048) return null;
        try { const value = JSON.parse(payload); return value && typeof value === 'object' && !Array.isArray(value) ? value : null; }
        catch { return null; }
    };
    const acknowledged = value => value &&
        Object.keys(value).sort().join(',') === 'channels,recommended_debounce_ms,recommended_jitter_ms,type' &&
        value.type === 'subscribed' && Array.isArray(value.channels) && value.channels.length <= 64 &&
        value.channels.every(item => typeof item === 'string' && item.length <= 256) &&
        new Set(value.channels).size === value.channels.length && value.channels.includes(channel) &&
        ['recommended_debounce_ms', 'recommended_jitter_ms'].every(key => Number.isSafeInteger(value[key]) && value[key] >= 0);
    const onSocket = socket => {
        if (closed) return;
        try {
            const url = new URL(socket.url()), target = new URL(origin);
            if (url.protocol !== (target.protocol === 'https:' ? 'wss:' : 'ws:') ||
                url.host !== target.host || url.pathname !== '/ws/events' || url.search || url.hash) return;
        } catch { if (active) fail(active); return; }
        if (sockets.size === 16) { overflow = true; if (active) fail(active); return; }
        const record = { socket, acknowledged: false, closed: false };
        if (active?.phase === 'new-socket') { active.next = record; active.phase = 'ack'; }
        const received = event => {
            if (closed) return;
            try {
                const value = decode(event);
                if (!value) return;
                if (acknowledged(value)) {
                    record.acknowledged = true;
                    if (active?.phase === 'ack' && active.next === record) active.phase = 'request';
                }
                if (Object.keys(value).sort().join(',') === 'error,message,type' && value.type === 'error' &&
                    typeof value.error === 'string' && typeof value.message === 'string' &&
                    ['auth_failed', 'auth_timeout', 'already_connected', 'not_authenticated',
                        'session_expired', 'session_revoked'].includes(value.error)) {
                    record.acknowledged = false;
                    if (active?.phase === 'terminal' && active.old === record) {
                        if (value.error !== 'auth_failed') fail(active);
                        else active.phase = 'new-socket';
                    } else if (active?.next === record) fail(active);
                }
            } catch { if (active) fail(active); }
        };
        const ended = () => {
            record.closed = true;
            if (active && ((active.phase === 'terminal' && active.old === record) || active.next === record)) fail(active);
        };
        socket.on('framereceived', received); socket.on('close', ended);
        sockets.set(socket, { received, ended, record });
    };
    const onRequest = request => {
        if (closed || active?.phase !== 'request') return;
        try {
            const url = new URL(request.url());
            if (url.origin !== origin || url.pathname !== '/api/frontend/applications' ||
                url.search || url.hash || request.method() !== 'GET') return;
            active.request = request; active.phase = 'response';
        } catch { fail(active); }
    };
    const onResponse = response => {
        if (closed || active?.phase !== 'response') return;
        const entry = active;
        try {
            if (response.request() !== entry.request) return;
            if (response.status() !== 200) { fail(entry); return; }
            entry.phase = 'body';
            let body, physical, bodyReady = false, physicalReady = false, startedDOM = false;
            const ready = () => {
                if (!bodyReady || !physicalReady || startedDOM || closed || active !== entry || entry.done) return;
                startedDOM = true; entry.phase = 'dom';
                const timeout = Math.ceil(entry.deadline - performance.now());
                if (timeout <= 0) { fail(entry); return; }
                let pending;
                try { pending = Promise.resolve(page.evaluate(authorityRecoveryDOM, timeout)); }
                catch (error) { pending = Promise.reject(error); }
                const handled = pending.then(value => {
                    if (value !== true) fail(entry); else finish(entry);
                }, error => { reject(entry, error); });
                tasks.add(handled);
            };
            try { body = Promise.resolve(response.body()); } catch (error) { body = Promise.reject(error); }
            try { physical = Promise.resolve(response.finished()); } catch (error) { physical = Promise.reject(error); }
            // Retain EACH physical settlement independently. A first failure
            // rejects acceptance immediately but cannot release the other task.
            const bodyTask = body.then(bytes => {
                if (!Buffer.isBuffer(bytes)) { fail(entry); return; }
                bodyReady = true; ready();
            }, error => { reject(entry, error); }).catch(error => { reject(entry, error); });
            const physicalTask = physical.then(error => {
                if (error !== null) { reject(entry, error); return; }
                physicalReady = true; ready();
            }, error => { reject(entry, error); }).catch(error => { reject(entry, error); });
            tasks.add(bodyTask); tasks.add(physicalTask);
        } catch { fail(entry); }
    };
    const onFailed = request => { if (active?.request === request) fail(active); };
    page.on('websocket', onSocket); page.on('request', onRequest);
    page.on('response', onResponse); page.on('requestfailed', onFailed);
    return Object.freeze({
        arm(deadline) {
            const current = [...sockets.values()].map(value => value.record)
                .filter(record => record.acknowledged && !record.closed);
            if (closed || overflow || active || arms === 3 || current.length !== 1 ||
                !Number.isFinite(deadline) || deadline <= performance.now()) throw failure();
            arms++;
            let resolve, reject;
            const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
            promise.catch(() => {});
            const entry = { old: current[0], next: null, phase: 'terminal', request: null,
                done: false, deadline, resolve, reject, timer: null };
            active = entry;
            entry.timer = setTimeout(() => fail(entry), Math.ceil(deadline - performance.now()));
            return Object.freeze({ wait: () => promise, stop: () => fail(entry) });
        },
        stop() {
            if (closed) return;
            closed = true;
            if (active) fail(active);
            page.off('websocket', onSocket); page.off('request', onRequest);
            page.off('response', onResponse); page.off('requestfailed', onFailed);
            for (const { record, received, ended } of sockets.values()) {
                record.socket.off('framereceived', received); record.socket.off('close', ended);
            }
            sockets.clear();
        },
        async drain(deadline) {
            await boundedStep(() => Promise.all([...tasks]), deadline);
            tasks.clear();
        },
    });
}

export function createResponseBodyRegistry() {
    const entries = new Set();
    let closed = false;
    return Object.freeze({
        capture(response) {
            if (closed || entries.size === 64) throw failure();
            const entry = { observed: false, outcome: null };
            entries.add(entry);
            // Invoke immediately: retaining Response alone does not retain its
            // lazy CDP body after navigation. Attach both handlers immediately.
            let pending;
            try { pending = Promise.resolve(response.body()); }
            catch (error) { pending = Promise.reject(error); }
            entry.settlement = pending.then(value => {
                entry.outcome = { ok: true, value }; return entry.outcome;
            }, error => {
                entry.outcome = { ok: false, error }; return entry.outcome;
            });
            return Object.freeze({ settlement: entry.settlement, async json() {
                entry.observed = true;
                const outcome = await entry.settlement;
                if (!outcome.ok) throw outcome.error;
                // Match Playwright Response.json without another CDP lookup.
                return JSON.parse(outcome.value.toString('utf8'));
            } });
        },
        async drain(deadline) {
            closed = true;
            // The journey closes its contexts/browser first. If that fails,
            // never turn this join into an unbounded cleanup wait.
            await boundedStep(() => Promise.all([...entries].map(entry => entry.settlement)), deadline);
            const errors = [...entries].filter(entry => !entry.observed && !entry.outcome.ok)
                .map(entry => entry.outcome.error);
            entries.clear();
            return errors;
        },
    });
}

export function observeReadLane(page, origin) {
    const pending = new Map(), waiters = new Set();
    let stopped = false, poisoned = false, draining = false, revision = 0, catalogCompleted = 0;
    const notify = () => { revision++; for (const wake of [...waiters]) wake(); };
    const poison = () => { poisoned = true; notify(); };
    const check = () => { if (stopped || poisoned) throw failure(); };
    const selected = request => {
        const url = new URL(request.url()), method = request.method();
        return url.origin === origin && ((method === 'GET' &&
            ['/api/frontend/applications', '/api/frontend/surfaces', '/api/frontend/float-layout'].includes(url.pathname)) ||
            (method === 'POST' && url.pathname === '/api/frontend/surfaces/admission'));
    };
    const onRequest = request => {
        if (stopped || poisoned) return;
        try {
            if (!selected(request)) return;
            if (pending.size === 64 || pending.has(request)) { poison(); return; }
            pending.set(request, { responding: false }); notify();
        } catch { poison(); }
    };
    const onResponse = response => {
        if (stopped) return;
        const request = response.request(), entry = pending.get(request);
        if (!entry) return;
        if (entry.responding) { poison(); return; }
        entry.responding = true;
        // Headers alone do not settle a native physical request owner.
        Promise.resolve().then(() => response.finished()).then(error => {
            if (stopped || pending.get(request) !== entry) return;
            pending.delete(request);
            if (error) { poison(); return; }
            if (request.method() === 'GET' && new URL(request.url()).pathname === '/api/frontend/applications' &&
                response.status() === 200) catalogCompleted++;
            notify();
        }).catch(() => { if (!stopped) poison(); });
    };
    const onFailed = request => { if (!stopped && pending.has(request)) { pending.delete(request); poison(); } };
    page.on('request', onRequest); page.on('response', onResponse); page.on('requestfailed', onFailed);
    return Object.freeze({
        catalogCompleted: () => catalogCompleted,
        async drain(deadline, { uiBarrier = true } = {}) {
            check();
            if (draining) throw failure();
            draining = true;
            try {
                for (;;) {
                    check();
                    while (pending.size) {
                        let wake;
                        const changed = new Promise(resolve => { wake = resolve; waiters.add(wake); });
                        try { await boundedStep(() => changed, deadline); }
                        finally { waiters.delete(wake); }
                        check();
                    }
                    const emptyRevision = revision;
                    if (uiBarrier) await boundedStep(() => page.evaluate(() => new Promise(resolve =>
                        requestAnimationFrame(() => resolve()))), deadline);
                    check();
                    if (!pending.size && revision === emptyRevision) return;
                    // A body-settled callback may issue a coalesced read before
                    // the real render barrier. Drain that read, not a sleep.
                }
            } finally { draining = false; }
        },
        stop() {
            if (stopped) return;
            stopped = true;
            page.off('request', onRequest); page.off('response', onResponse); page.off('requestfailed', onFailed);
            pending.clear(); notify();
        },
    });
}

export async function withFrozenReadLane(page, lane, deadline, action) {
    const context = page.context();
    let session, acquisition, attemptedFreeze = false, result, primary, hasPrimary = false;
    const cleanup = [];
    const cleanupStep = run => {
        // Cleanup gets its own finite per-step ACK budget, never an extension
        // of the original action deadline. Dispatch even after prior timeout.
        const pending = Promise.resolve(run());
        pending.catch(() => {});
        return boundedStep(() => pending, performance.now() + 2000);
    };
    try {
        session = await boundedStep(() => {
            acquisition = Promise.resolve(context.newCDPSession(page));
            return acquisition;
        }, deadline);
        attemptedFreeze = true;
        await boundedStep(() => session.send('Page.setWebLifecycleState', { state: 'frozen' }), deadline);
        // No requestAnimationFrame can settle while the page is frozen.
        await lane.drain(deadline, { uiBarrier: false });
        result = await boundedStep(action, deadline); // Exactly one mutation.
    } catch (error) { primary = error; hasPrimary = true; }
    finally {
        if (session) {
            if (attemptedFreeze) {
                try { await cleanupStep(() => session.send('Page.setWebLifecycleState', { state: 'active' })); }
                catch (error) { cleanup.push(error); }
            }
            try { await cleanupStep(() => session.detach()); }
            catch (error) { cleanup.push(error); }
        } else {
            // A timed-out acquisition can still resolve later. Retain a handled
            // detach continuation and close the already outer-owned context;
            // no late session can revive a successfully closed context.
            const late = acquisition?.then(acquired => cleanupStep(() => acquired.detach()), () => {});
            late?.catch(() => {});
            try { await cleanupStep(() => context.close()); }
            catch (error) { cleanup.push(error); }
            if (late) {
                try { await cleanupStep(() => late); }
                catch (error) { cleanup.push(error); }
            }
        }
    }
    if (hasPrimary && !cleanup.length) throw primary;
    if (hasPrimary || cleanup.length) throw new AggregateError([...(hasPrimary ? [primary] : []), ...cleanup],
        'installed float background-tab coordination and cleanup failed');
    return result;
}
