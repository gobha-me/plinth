// Native typed-transport fixture, not a shipped navigation/tray implementation.
import * as preact from 'preact';
import * as sdk from '@plinth/frontend/sdk';
import { label } from './label.js';

export async function capturedProbe() {
    const dynamic = await import('./detail.js');
    return { label, detail: dynamic.detail, preact, sdk };
}

export default function typedFixture() {
    return function TypedFixture() {
        return preact.h('p', { class: 'installed-typed-transport-fixture' },
            'Typed transport fixture; navigation and tray chrome are not exercised here.');
    };
}
