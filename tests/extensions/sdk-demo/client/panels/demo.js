// sdk_demo/client/panels/demo.js — exercise the Panel SDK + Client SDK
// end-to-end per ICD-0.6.3 §13.5 manual FE smoke gate.
//
// Imports `@plinth/frontend/sdk` via the import-map declared in the
// shell's index.html. `plinth.panel` is injected as the first argument
// to the default-export factory by the shell's panel loader.

import { h } from 'preact';
import { useState, useEffect } from 'preact/hooks';
import { subscribe, useData } from '@plinth/frontend/sdk';

export default function demo(plinthPanel) {
    const channel = 'plinth:data:ext_shell.user_preferences';
    const lifecycle = { activations: 0, deactivations: 0, shortcuts: 0 };
    const lifecycleListeners = new Set();
    const changed = () => {
        for (const listener of lifecycleListeners) listener({ ...lifecycle });
    };
    plinthPanel.onActivate(() => {
        lifecycle.activations++;
        changed();
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] onActivate');
    });
    plinthPanel.onDeactivate(() => {
        lifecycle.deactivations++;
        changed();
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] onDeactivate');
    });
    plinthPanel.registerShortcut('Ctrl+Shift+D', () => {
        lifecycle.shortcuts++;
        changed();
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] Ctrl+Shift+D pressed');
    });

    return function DemoPanel() {
        const { data: preference, error: preferenceError, loading } = useData(
            channel, {
                initialData: { value: '(loading)' },
                snapshot: { capability: 'shell.preferences.get',
                            args: { key: 'shell.theme' } },
            });
        const theme = preferenceError
            ? '(error: ' + preferenceError.code + ')'
            : (preference?.value ?? '(unset)');
        const [envelopes, setEnv] = useState(0);
        const [lastSequence, setSequence] = useState('none');
        const [ready, setReady] = useState(false);
        const [rawError, setRawError] = useState(null);
        const [counts, setCounts] = useState({ ...lifecycle });
        const [throwing, setThrow] = useState(false);

        useEffect(() => {
            lifecycleListeners.add(setCounts);
            setCounts({ ...lifecycle });
            return () => lifecycleListeners.delete(setCounts);
        }, []);
        useEffect(() => {
            const unsub = subscribe(channel, (env) => {
                setEnv(prev => prev + 1);
                setSequence(String(env.payload?.seq ?? 'none'));
            }, {
                onReady: () => { setReady(true); setRawError(null); },
                onError: error => setRawError(error.code || error.name),
            });
            return unsub;
        }, []);

        if (throwing) {
            // Boundary smoke: a deliberate throw bubbles to the
            // shell panel boundary, which calls shell.audit.emit and
            // renders the contained fallback UI. Verify in plinth.audit_log:
            //   SELECT timestamp, data FROM plinth.audit_log
            //   WHERE action='ext.shell.frontend.boundary.caught'
            //   ORDER BY timestamp DESC LIMIT 5;
            throw new Error('sdk_demo deliberate boundary throw');
        }

        return h('div', { id: 'sdk-demo-panel', style: 'padding: 1rem; font-family: var(--mono);' },
            h('h2', null, 'SDK Demo Panel'),
            h('div', null, 'theme: ', h('strong', { id: 'sdk-demo-theme' }, String(theme))),
            h('output', { id: 'sdk-demo-loading' }, String(loading)),
            h('output', { id: 'sdk-demo-error' }, preferenceError?.code || preferenceError?.name || 'none'),
            h('div', null, 'envelopes received: ',
                h('strong', { id: 'sdk-demo-raw-count' }, String(envelopes))),
            h('output', { id: 'sdk-demo-raw-seq' }, lastSequence),
            h('output', { id: 'sdk-demo-raw-ready' }, String(ready)),
            h('output', { id: 'sdk-demo-raw-error' }, rawError || 'none'),
            h('output', { id: 'sdk-demo-activations' }, String(counts.activations)),
            h('output', { id: 'sdk-demo-deactivations' }, String(counts.deactivations)),
            h('output', { id: 'sdk-demo-shortcuts' }, String(counts.shortcuts)),
            h('button',
                { id: 'sdk-demo-boundary-throw', onClick: () => setThrow(true),
                  style: 'margin-top: 0.5rem;' },
                'Trigger boundary throw'));
    };
}
