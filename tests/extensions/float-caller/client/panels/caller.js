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
    return function Caller() {
        const [record, setRecord] = hooks.useState('fixture-record-A');
        const [status, setStatus] = hooks.useState('idle');
        const mounted = hooks.useRef(false);
        hooks.useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
        async function open(liveOnly = false) {
            setStatus('opening');
            try {
                const result = await panel.openFloat(liveOnly ? 'application/x-plinth-float-live' :
                    'application/x-plinth-float-fixture', { record_id: record,
                    ...(liveOnly ? { live_nonce: 'test-only-required-transient' } : {}) });
                if (mounted.current) setStatus('opened:' + result.floatId);
            } catch (error) {
                if (mounted.current) setStatus('refused:' +
                    (typeof error?.code === 'string' ? error.code : 'unknown'));
            }
        }
        return preact.h('section', { id: 'installed-float-caller' },
            preact.h('h2', null, 'Installed Float Caller'),
            preact.h('label', null, 'Fixture record ID', preact.h('input', {
                id: 'installed-float-record', value: record, onInput: event => setRecord(event.target.value) })),
            preact.h('button', { id: 'installed-float-open', type: 'button', onClick: () => open() }, 'Open fixture float'),
            preact.h('button', { id: 'installed-float-open-live', type: 'button', onClick: () => open(true) }, 'Open live-only fixture'),
            preact.h('output', { id: 'installed-float-open-result', role: 'status' }, status));
    };
}
