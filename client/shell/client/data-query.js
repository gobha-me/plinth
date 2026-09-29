// Private request/view capture shared by call and the smart-data controller.
// Captured argument bytes are request identity, never logging or wire metadata.
function canonical(value) {
    if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
    if (value !== null && typeof value === 'object') {
        return `{${Object.keys(value).sort().map(key =>
            `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`;
    }
    return JSON.stringify(value);
}

export function prepareCapabilityRequest(capability, args) {
    const encoded = encodeURIComponent(capability);
    const body = JSON.stringify({ args: args === undefined ? null : args });
    return Object.freeze({
        capability: decodeURIComponent(encoded), url: `/api/cap/${encoded}`, body,
        identity: canonical(JSON.parse(body)),
    });
}

export function isViewId(value) {
    return typeof value === 'string' || Number.isSafeInteger(value);
}

function record(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function captureSupportedView(view) {
    if (!record(view) || typeof view.key !== 'string' || !view.key || view.complete !== true ||
        Object.keys(view).some(key => !['key', 'complete', 'where', 'insertRows'].includes(key)) ||
        (view.insertRows !== undefined && typeof view.insertRows !== 'function')) return null;
    let where = null;
    if (view.where !== undefined) {
        if (!record(view.where) || Object.keys(view.where).length !== 1) return null;
        if (Object.hasOwn(view.where, 'eq') && isViewId(view.where.eq)) {
            where = Object.freeze({ eq: view.where.eq });
        } else if (Object.hasOwn(view.where, 'in') && Array.isArray(view.where.in) &&
            view.where.in.every(isViewId) && new Set(view.where.in).size === view.where.in.length) {
            const ids = [...view.where.in].sort((a, b) =>
                canonical(a).localeCompare(canonical(b)));
            where = Object.freeze({ in: Object.freeze(ids) });
        } else return null;
    }
    return Object.freeze({ key: view.key, complete: true, where, insertRows: view.insertRows });
}

export function captureView(view) {
    try { return captureSupportedView(view); }
    catch { return null; }
}

function errorIdentity(error) {
    if (!error) return null;
    return [error.name, error.message, error.cause?.name, error.cause?.message];
}

export function sameQuery(a, b) {
    if (!a || !b || a.channel !== b.channel || a.scope !== b.scope ||
        a.snapshot?.url !== b.snapshot?.url || a.snapshot?.identity !== b.snapshot?.identity ||
        JSON.stringify(errorIdentity(a.preparationError)) !==
            JSON.stringify(errorIdentity(b.preparationError))) return false;
    if (a.view === b.view) return true;
    if (!a.view || !b.view) return false;
    return a.view.key === b.view.key && a.view.insertRows === b.view.insertRows &&
        canonical(a.view.where) === canonical(b.view.where);
}
