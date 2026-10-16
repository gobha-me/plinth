import assert from 'node:assert/strict';
import test from 'node:test';
import { createFloatResolver } from '../../client/shell/client/panels/float-resolver.js';
import { target } from './float-production-fixtures.mjs';
function fixture({ defaults = [], personal = null, admin = null, administrator = null, earliest = target(), fail = null } = {}) {
    const calls = [];
    const transport = {
        async personalDefaults() { calls.push('personal'); if (fail === 'personal') throw new Error('unavailable'); return defaults; },
        async catalog(query) {
            calls.push(['catalog', query]); if (fail === 'admin' && query.include_admin_default) throw new Error('unavailable');
            return { targets: earliest ? [earliest] : [], next_cursor: null, admin_default_candidate: admin };
        },
        async admit([request]) {
            calls.push(['admit', request]);
            const available = request.application_id === 'personal' ? personal : administrator;
            return [{ index: 0, status: available ? 'available' : 'unavailable', ...(available ? { target: available } : {}) }];
        },
    };
    return { calls, resolver: createFloatResolver(transport) };
}
const choice = application_id => ({ application_id, panel_id: 'preview', content_type: 'text/plain' });

test('usable personal choice performs zero catalog or administrator reads', async () => {
    const expected = target({ application_id: 'personal' });
    const f = fixture({ defaults: [choice('personal')], personal: expected, fail: 'admin' });
    assert.equal(await f.resolver.resolveAutomatic('Text/Plain'), expected);
    assert.equal(f.calls.length, 2); assert.equal(f.calls[0], 'personal');
});
test('denied personal advances to usable administrator even outside first catalog page', async () => {
    const expected = target({ application_id: 'adminapp' });
    const f = fixture({ defaults: [choice('personal')], admin: choice('adminapp'), administrator: expected });
    assert.equal(await f.resolver.resolveAutomatic('text/plain'), expected);
    assert.equal(f.calls[2][1].limit, 1); assert.equal(f.calls[2][1].include_admin_default, true);
    assert.equal(f.calls[3][1].expected_generation, null);
    assert.equal(Object.hasOwn(f.calls[3][1], 'capability'), false);
});
test('affirmative absence or unusable administrator chooses only earliest; unknown never does', async () => {
    for (const admin of [null, choice('adminapp')]) {
        const expected = target({ logical_install_order: '18446744073709551615' });
        const f = fixture({ admin, earliest: expected });
        assert.equal(await f.resolver.resolveAutomatic('text/plain'), expected);
    }
    for (const fail of ['personal', 'admin']) {
        const f = fixture({ fail }); await assert.rejects(f.resolver.resolveAutomatic('text/plain'));
        assert.equal(f.calls.some(call => call[0] === 'admit'), false);
    }
    await assert.rejects(fixture({ earliest: null }).resolver.resolveAutomatic('text/plain'), { code: 'missing-handler' });
});
test('manual alternatives omit administrator lane even during administrator outage', async () => {
    const f = fixture({ fail: 'admin', administrator: target({ application_id: 'manual' }) });
    await f.resolver.listManual('text/plain');
    assert.equal(Object.hasOwn(f.calls[0][1], 'include_admin_default'), false);
    assert.equal((await f.resolver.selectManual(choice('manual'), 'text/plain')).application_id, 'manual');
    assert.equal(f.calls.some(call => call === 'personal'), false);
});
