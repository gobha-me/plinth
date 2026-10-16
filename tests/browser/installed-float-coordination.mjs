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
