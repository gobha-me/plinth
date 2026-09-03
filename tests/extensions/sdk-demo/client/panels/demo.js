// sdk_demo/client/panels/demo.js — exercise the Panel SDK + Client SDK
// end-to-end per ICD-0.6.3 §13.5 manual FE smoke gate.
//
// Imports `@plinth/frontend/sdk` via the import-map declared in the
// shell's index.html. `plinth.panel` is injected as the first argument
// to the default-export factory by the shell's panel loader.

import { h } from 'preact';
import { useState, useEffect } from 'preact/hooks';
import { call, subscribe } from '@plinth/frontend/sdk';

export default function demo(plinthPanel) {
    plinthPanel.onActivate(() => {
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] onActivate');
    });
    plinthPanel.onDeactivate(() => {
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] onDeactivate');
    });
    plinthPanel.registerShortcut('Ctrl+Shift+D', () => {
        // eslint-disable-next-line no-console
        console.log('[sdk_demo] Ctrl+Shift+D pressed');
    });

    return function DemoPanel() {
        const [theme, setTheme]   = useState('(loading)');
        const [envelopes, setEnv] = useState([]);
        const [throwing, setThrow] = useState(false);

        useEffect(() => {
            call('shell.preferences.get', { key: 'shell.theme' }).then(
                (r) => setTheme(r.value ?? '(unset)'),
                (e) => setTheme('(error: ' + e.code + ')'));
            const unsub = subscribe('sdk_demo:test', (env) => {
                setEnv((prev) => [...prev.slice(-4), env]);
            });
            return unsub;
        }, []);

        if (throwing) {
            // Boundary smoke: a deliberate throw bubbles to the
            // top-level boundary, which calls shell.audit.emit and
            // renders the fallback UI. Verify in plinth.audit_log:
            //   SELECT timestamp, data FROM plinth.audit_log
            //   WHERE action='ext.shell.frontend.boundary.caught'
            //   ORDER BY timestamp DESC LIMIT 5;
            throw new Error('sdk_demo deliberate boundary throw');
        }

        return h('div', { style: 'padding: 1rem; font-family: var(--mono);' },
            h('h2', null, 'SDK Demo Panel'),
            h('div', null, 'theme: ', h('strong', null, String(theme))),
            h('div', null, 'envelopes received: ',
                h('strong', null, String(envelopes.length))),
            h('button',
                { onClick: () => setThrow(true),
                  style: 'margin-top: 0.5rem;' },
                'Trigger boundary throw'));
    };
}
