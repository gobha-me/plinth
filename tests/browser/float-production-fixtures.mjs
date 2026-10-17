export const generation = '12345678-1234-4234-8234-123456789abc';
export const artifact = 'a'.repeat(64);
export const location = { href: 'https://plinth.test/app/', origin: 'https://plinth.test' };
export const plain = value => JSON.parse(JSON.stringify(value));
export const drain = async () => { for (let count = 0; count < 20; count++) await Promise.resolve(); };
export function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
export function clock() {
    let now = 0, next = 0;
    const timers = new Map();
    return { timers, now: () => now,
        setTimeout(callback, delay) { const token = ++next; timers.set(token, { callback, at: now + delay }); return token; },
        clearTimeout(token) { timers.delete(token); },
        advance(delay) {
            const end = now + delay;
            while (true) {
                const due = [...timers].filter(([, timer]) => timer.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
                if (!due) break;
                now = due[1].at; timers.delete(due[0]); due[1].callback();
            }
            now = end;
        } };
}
export function target(overrides = {}) {
    return { application_id: 'notes', panel_id: 'preview', type: 'float', generation,
        version: '0.6.7', title: 'Preview', icon: 'note',
        module_url: `/_plinth/typed/${generation}/${artifact}/preview/client/panel.js`,
        capability: 'notes:1:preview', content_types: ['text/plain'],
        context_schema: { version: 1, fields: { record_id: { required: true, max_bytes: 32, meaning: 'stable fake ID' } },
            persist_keys: ['record_id'] }, logical_install_order: '1',
        persistence_contract: { projection_contract: { id: 'record', version: 1 }, artifact_digest: artifact,
            schema_digest: 'b'.repeat(64), approved_keys: ['record_id'], policy_revision: '1' }, ...overrides };
}
export function entry(id = 'fake-one', overrides = {}) {
    return { application_id: 'notes', panel_id: 'preview', capability: 'notes:1:preview',
        context: { record_id: id }, presentation: 'shown', maximized: false,
        geometry: { x: 0, y: 0, width: 640, height: 480 }, ...overrides };
}
export const snapshot = (...entries) => ({ version: 1, floats: entries });
