import * as preact from 'preact';
import * as hooks from 'preact/hooks';
import * as sdk from '@plinth/frontend/sdk';
import { label } from './label.js';

export default function preview(panel) {
    const witness = globalThis.__plinthInstalledFloats ??= {
        sdk: new Set(), preact: new Set(), hooks: new Set(), factories: 0,
        active: new Map(), unmounted: [], activations: 0, deactivations: 0,
    };
    witness.sdk.add(sdk); witness.preact.add(preact); witness.hooks.add(hooks);
    witness.factories++;
    const initial = panel.getContext(), instance = crypto.randomUUID();
    const incarnations = witness.incarnations ??= new Map();
    const incarnation = { activations: 0, deactivations: 0, mounts: 0, unmounts: 0 };
    incarnations.set(instance, incarnation);
    panel.onActivate(() => { witness.activations++; incarnation.activations++; });
    panel.onDeactivate(() => { witness.deactivations++; incarnation.deactivations++; });
    return function Preview() {
        const [text, setText] = hooks.useState('');
        const [detail, setDetail] = hooks.useState('loading');
        const [crashed, setCrashed] = hooks.useState(false);
        hooks.useEffect(() => {
            let current = true;
            incarnation.mounts++;
            witness.active.set(instance, initial.record_id);
            import('./detail.js').then(module => { if (current) setDetail(module.detail); },
                () => { if (current) setDetail('failed'); });
            return () => {
                current = false; incarnation.unmounts++;
                witness.active.delete(instance); witness.unmounted.push(instance);
            };
        }, []);
        // An ordinary installed component's render error, not a kernel or
        // shell fault-injection seam. A new Retry incarnation starts clean.
        if (crashed) throw new Error('controlled installed float render failure');
        return preact.h('section', { class: 'installed-float-provider', 'data-instance': instance,
            'data-record-id': initial.record_id, 'data-live-only': String(Object.hasOwn(initial, 'live_nonce')),
            'data-context-stable': String(panel.getContext() === initial) },
            preact.h('h3', null, 'Installed provider fixture'),
            preact.h('output', { class: 'installed-float-record-id' }, initial.record_id),
            preact.h('p', { class: 'installed-float-relative' }, label),
            preact.h('p', { class: 'installed-float-dynamic' }, detail),
            preact.h('label', null, 'Unsaved fixture text', preact.h('input', {
                class: 'installed-float-unsaved', value: text, onInput: event => {
                    setText(event.target.value); panel.setDirty(event.target.value.length > 0);
                } })),
            preact.h('button', { type: 'button', onClick: () => panel.requestFocus() }, 'Request fixture focus'),
            preact.h('button', { type: 'button', onClick: () => setCrashed(true) }, 'Crash fixture render'));
    };
}
