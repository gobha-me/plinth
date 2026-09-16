import assert from 'node:assert/strict';
import test from 'node:test';
import {
    choosePanel, normalizeCatalog, normalizeLauncherPreference,
    orderApplications, updatePreference,
} from '../../client/shell/client/launcher/model.js';

const catalog = normalizeCatalog({ schema_version: 1, applications: [
    { id: 'notes', generation: 'g1', version: '1.0.0', title: 'Notes', panels: [
        { id: 'editor', title: 'Editor', module_url: '/ext/notes/1.0.0/panels/editor.js' },
        { id: 'search', title: 'Search', module_url: '/ext/notes/1.0.0/panels/search.js' },
    ] },
    { id: 'files', generation: 'g2', version: '1.0.0', title: 'Files', panels: [
        { id: 'list', title: 'List', module_url: '/ext/files/1.0.0/panels/list.js' },
    ] },
] });

test('catalog validation is fail closed', () => {
    assert.equal(catalog.length, 2);
    assert.throws(() => normalizeCatalog({ schema_version: 1, applications: [
        { id: 'notes', generation: 'g', version: '1', title: 'Notes', panels: [] },
    ] }));
    assert.throws(() => normalizeCatalog({ schema_version: 1, applications: [
        { id: 'notes', generation: 'g', version: '1', title: 'Notes', panels: [
            { id: 'x', title: 'X', module_url: 'https://foreign.invalid/x.js' },
        ] },
    ] }));
    assert.equal(normalizeCatalog({ schema_version: 1, applications: [
        { id: 'unicode', generation: 'g', version: '1', title: '😀'.repeat(128), panels: [
            { id: 'main', title: '😀'.repeat(256), module_url: '/ext/unicode/1/panels/main.js' },
        ] },
    ] })[0].title, '😀'.repeat(128));
    assert.throws(() => normalizeCatalog({ schema_version: 1, applications: [
        { id: 'unicode', generation: 'g', version: '1', title: '😀'.repeat(129), panels: [
            { id: 'main', title: 'Main', module_url: '/ext/unicode/1/panels/main.js' },
        ] },
    ] }));
});

test('preference normalization bounds, deduplicates, orders and falls back', () => {
    const preference = normalizeLauncherPreference({
        version: 1,
        last_application: 'notes',
        last_panels: { notes: 'search', 'Bad App': 'x' },
        application_order: ['files', 'files', 'missing'],
    });
    assert.deepEqual(orderApplications(catalog, preference).map(app => app.id), ['files', 'notes']);
    assert.equal(choosePanel(catalog[0], preference).id, 'search');
    assert.equal(choosePanel(catalog[1], preference).id, 'list');
    assert.deepEqual(updatePreference(preference, 'files', 'list').last_panels,
        { notes: 'search', files: 'list' });
    assert.deepEqual(normalizeLauncherPreference(null),
        { version: 1, last_application: null, last_panels: {}, application_order: [] });
    assert.deepEqual(normalizeLauncherPreference({
        version: 1, last_application: undefined, last_panels: { notes: null },
        application_order: [null, undefined],
    }), { version: 1, last_application: null, last_panels: {}, application_order: [] });
});
