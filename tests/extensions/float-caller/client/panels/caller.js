import * as preact from 'preact';
import * as hooks from 'preact/hooks';
import * as sdk from '@plinth/frontend/sdk';

export default function caller(panel) {
    // Fake fixture witnesses only: no credentials, user data, URLs or authority.
    const witness = globalThis.__plinthInstalledFloats ??= {
        sdk: new Set(), preact: new Set(), hooks: new Set(), factories: 0,
        active: new Map(), unmounted: [], activations: 0, deactivations: 0,
    };
    witness.sdk.add(sdk); witness.preact.add(preact); witness.hooks.add(hooks);
    // Fake bounded lifecycle diagnostics only; never retain context or results.
    const callers = witness.callers ??= { instances: [], events: 0, overflow: false, lastStarted: null };
    const codes = new Set(['none', 'opened', 'missing-handler', 'authority-unavailable', 'cancelled',
        'invalid-input', 'limit-refused', 'request-busy', 'load-failed', 'target-unavailable',
        'reload-required', 'session-ended', 'unknown']);
    const count = (entry, key) => {
        if (!entry) return;
        entry[key] = Math.min(64, entry[key] + 1);
        if (callers.events === 64) callers.overflow = true;
        callers.events = Math.min(64, callers.events + 1);
    };
    const code = error => {
        try {
            const value = Object.getOwnPropertyDescriptor(error, 'code')?.value;
            return codes.has(value) ? value : 'unknown';
        } catch { return 'unknown'; }
    };
    return function Caller() {
        const [record, setRecord] = hooks.useState('fixture-record-A');
        const [status, setStatus] = hooks.useState('idle');
        const mounted = hooks.useRef(false);
        const observation = hooks.useRef(null);
        if (observation.current === null) {
            if (callers.instances.length < 64) {
                observation.current = { incarnation: callers.instances.length + 1, mounted: false,
                    mounts: 0, unmounts: 0, open_starts: 0, resolved: 0, rejected: 0, suppressed: 0, last_code: 'none' };
                callers.instances.push(observation.current);
            } else callers.overflow = true;
        }
        hooks.useEffect(() => {
            mounted.current = true;
            if (observation.current) observation.current.mounted = true;
            count(observation.current, 'mounts');
            return () => {
                mounted.current = false;
                if (observation.current) observation.current.mounted = false;
                count(observation.current, 'unmounts');
            };
        }, []);
        async function open(liveOnly = false) {
            count(observation.current, 'open_starts');
            callers.lastStarted = observation.current?.incarnation ?? null;
            setStatus('opening');
            try {
                const result = await panel.openFloat(liveOnly ? 'application/x-plinth-float-live' :
                    'application/x-plinth-float-fixture', { record_id: record,
                    ...(liveOnly ? { live_nonce: 'test-only-required-transient' } : {}) });
                count(observation.current, 'resolved');
                if (observation.current) observation.current.last_code = 'opened';
                if (!mounted.current) count(observation.current, 'suppressed');
                if (mounted.current) setStatus('opened:' + result.floatId);
            } catch (error) {
                count(observation.current, 'rejected');
                if (observation.current) observation.current.last_code = code(error);
                if (!mounted.current) count(observation.current, 'suppressed');
                if (mounted.current) setStatus('refused:' +
                    (typeof error?.code === 'string' ? error.code : 'unknown'));
            }
        }
        return preact.h('section', { id: 'installed-float-caller',
            'data-caller-incarnation': observation.current?.incarnation ?? '' },
            preact.h('h2', null, 'Installed Float Caller'),
            preact.h('label', null, 'Fixture record ID', preact.h('input', {
                id: 'installed-float-record', value: record, onInput: event => setRecord(event.target.value) })),
            preact.h('button', { id: 'installed-float-open', type: 'button', onClick: () => open() }, 'Open fixture float'),
            preact.h('button', { id: 'installed-float-open-live', type: 'button', onClick: () => open(true) }, 'Open live-only fixture'),
            preact.h('output', { id: 'installed-float-open-result', role: 'status' }, status));
    };
}
