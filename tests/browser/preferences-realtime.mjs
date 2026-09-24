import assert from 'node:assert/strict';
import { createHash, createHmac, randomBytes, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
assert(baseURL, 'PLINTH_BASE_URL must name a task-owned production kernel');
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env['PLINTH_PG_' + suffix], `PLINTH_PG_${suffix} is required`);
    pgEnv['PG' + suffix] = process.env['PLINTH_PG_' + suffix];
}
function sql(statement) {
    return execFileSync('psql', ['-X', '-v', 'ON_ERROR_STOP=1', '-A', '-t', '-c', statement],
        { env: pgEnv, encoding: 'utf8', timeout: 15000 }).trim();
}
async function waitFor(read, message) {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
        const value = read();
        if (value) return value;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw new Error(message);
}

const channel = 'plinth:data:ext_shell.user_preferences';
const key = 'browser.preference_realtime';
const user = randomUUID();
const group = randomUUID();
const sessions = [randomUUID(), randomUUID()];
const tokens = [randomBytes(32).toString('base64url'), randomBytes(32).toString('base64url')];
const hashes = tokens.map(token => createHash('sha256').update(token).digest('hex'));
const csrf = createHmac('sha256', tokens[0]).update('plinth.csrf.v1').digest('base64url');
// The grant is test-only: this table-wide channel must not become a default
// grant for normal users. Both browser sessions belong to the same user.
sql(`INSERT INTO plinth.users(id,username,password_hash) VALUES
    ('${user}','preference-${user}','not-a-password-hash');
    INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES
    ('${sessions[0]}','${user}','${hashes[0]}'),
    ('${sessions[1]}','${user}','${hashes[1]}');
    INSERT INTO plinth.groups(id,name) VALUES ('${group}','preference-${group}');
    INSERT INTO plinth.group_members(group_id,user_id) VALUES ('${group}','${user}');
    INSERT INTO plinth.rbac_rules(rule,namespace,description,extension_name)
    VALUES ('shell.realtime.subscribe','shell','Browser preference regression','shell')
    ON CONFLICT DO NOTHING;
    INSERT INTO plinth.group_rules(group_id,rule_id)
    SELECT '${group}',id FROM plinth.rbac_rules WHERE rule='shell.realtime.subscribe';`);
assert.equal(sql(`SELECT count(*) FROM plinth.group_members gm
    JOIN plinth.group_rules gr USING(group_id) JOIN plinth.rbac_rules r ON r.id=gr.rule_id
    WHERE gm.user_id='${user}' AND r.rule='kernel.admin'`), '0');

const browser = await chromium.launch({
    executablePath: process.env.PLINTH_BROWSER || undefined,
    args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
});
try {
    const context = await browser.newContext();
    await context.addCookies([
        { name: 'plinth_session', value: tokens[0], url: baseURL,
            httpOnly: true, sameSite: 'Strict', secure: baseURL.startsWith('https:') },
        { name: 'plinth_csrf', value: csrf, url: baseURL,
            httpOnly: false, sameSite: 'Strict', secure: baseURL.startsWith('https:') },
    ]);
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(baseURL + '/app/');
    await page.evaluate(async channel => {
        const sdk = await import('@plinth/frontend/sdk');
        window.__preferenceSdk = sdk;
        window.__preferenceEvents = [];
        window.__preferenceErrors = [];
        window.__preferenceReady = false;
        window.__removePreference = sdk.subscribe(channel,
            event => window.__preferenceEvents.push(event), {
                onReady: () => { window.__preferenceReady = true; },
                onError: error => window.__preferenceErrors.push(error.code),
            });
    }, channel);
    await page.waitForFunction(() => window.__preferenceReady || window.__preferenceErrors.length);
    assert.deepEqual(await page.evaluate(() => window.__preferenceErrors), []);
    const baseline = Number(sql(`SELECT coalesce(max(seq),0) FROM plinth.events
        WHERE channel='${channel}'`));

    async function writeAndObserve(value, op, expectedValue) {
        const prior = Number(sql(`SELECT coalesce(max(seq),0) FROM plinth.events
            WHERE channel='${channel}'`));
        const outcome = await page.evaluate(({ key, value, remove }) =>
            window.__preferenceSdk.call('shell.preferences.set',
                remove ? { key } : { key, value }),
        { key, value, remove: value === undefined });
        assert.equal(outcome.ok, true);
        if (value === undefined) assert.equal(outcome.deleted, true);

        const event = await waitFor(() => {
            const row = sql(`SELECT json_build_object('seq',seq,'payload',payload)::text
                FROM plinth.events WHERE channel='${channel}' AND seq>${prior}
                ORDER BY seq LIMIT 1`);
            return row ? JSON.parse(row) : null;
        }, `${op} did not persist a preference realtime event`);
        assert(event.seq > prior);
        assert.equal(event.payload.channel, channel);
        assert.equal(event.payload.schema, 'ext_shell');
        assert.equal(event.payload.table, 'user_preferences');
        assert.deepEqual(event.payload.ops, [
            { op: 'insert', count: op === 'insert' ? 1 : 0 },
            { op: 'update', count: 0 },
            { op: 'delete', count: op === 'delete' ? 1 : 0 },
        ]);
        await page.waitForFunction(seq => window.__preferenceEvents.some(
            frame => frame.payload?.seq === seq), event.seq);
        const live = await page.evaluate(seq => window.__preferenceEvents.find(
            frame => frame.payload?.seq === seq), event.seq);
        assert.equal(live.channel, channel);
        assert.deepEqual(live.payload.ops, event.payload.ops);
        const current = await page.evaluate(key =>
            window.__preferenceSdk.call('shell.preferences.get', { key }), key);
        if (value === undefined) assert.equal(Object.hasOwn(current, 'value'), false);
        else assert.equal(current.value, expectedValue);
        return event.seq;
    }

    const created = await writeAndObserve('created', 'insert', 'created');
    const updated = await writeAndObserve('updated', 'insert', 'updated');
    const deleted = await writeAndObserve(undefined, 'delete');
    assert(created < updated && updated < deleted);
    assert.equal(await page.evaluate(() => window.__preferenceEvents.length), 3);
    assert.deepEqual(await page.evaluate(() => window.__preferenceErrors), []);

    // Replay uses another session because the server permits one socket per
    // session. It must return the same three durable sequence numbers.
    const replayContext = await browser.newContext();
    await replayContext.addCookies([{ name: 'plinth_session', value: tokens[1],
        url: baseURL, httpOnly: true, sameSite: 'Strict', secure: baseURL.startsWith('https:') }]);
    const replayPage = await replayContext.newPage();
    // An inert same-origin page avoids the shell launcher's own SDK socket
    // competing with this raw replay socket for the second session.
    await replayPage.goto(baseURL + '/healthz');
    const replayed = await replayPage.evaluate(({ channel, baseline }) => new Promise((resolve, reject) => {
        const socket = new WebSocket(location.origin.replace(/^http/, 'ws') + '/ws/events');
        const frames = [];
        const timer = setTimeout(() => finish(new Error('preference replay timed out')), 15000);
        function finish(error) {
            clearTimeout(timer);
            socket.close();
            if (error) reject(error);
            else resolve(frames);
        }
        socket.onmessage = event => {
            const frame = JSON.parse(event.data);
            if (frame.type === 'connected') {
                socket.send(JSON.stringify({ type: 'subscribe', channels: [channel], since_seq: baseline }));
            } else if (frame.type === 'replay') {
                frames.push(frame.envelope);
            } else if (frame.type === 'replay_done') {
                finish();
            } else if (frame.type === 'error') {
                finish(new Error(`preference replay rejected: ${frame.error}`));
            }
        };
        socket.onerror = () => finish(new Error('preference replay WebSocket failed'));
    }), { channel, baseline });
    assert.deepEqual(replayed.map(frame => frame.seq), [created, updated, deleted]);
    assert(replayed.every(frame => frame.channel === channel));
    assert.deepEqual(replayed.map(frame => frame.ops), [
        [{ op: 'insert', count: 1 }, { op: 'update', count: 0 }, { op: 'delete', count: 0 }],
        [{ op: 'insert', count: 1 }, { op: 'update', count: 0 }, { op: 'delete', count: 0 }],
        [{ op: 'insert', count: 0 }, { op: 'update', count: 0 }, { op: 'delete', count: 1 }],
    ]);
    await replayContext.close();
    await page.evaluate(() => window.__removePreference());
    assert.deepEqual(errors, []);
    await context.close();
    console.log('PASS preference create, update, delete durable events, live browser SDK, and replay');
} finally {
    await browser.close();
}
