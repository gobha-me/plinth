import assert from 'node:assert/strict';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
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
function literal(value) { return "'" + value.replaceAll("'", "''") + "'"; }
const user = randomUUID();
const group = randomUUID();
const session = randomUUID();
const token = randomBytes(32).toString('base64url');
const hash = createHash('sha256').update(token).digest('hex');
const channel = 'plinth:ext:browser:update';
const keepChannel = 'plinth:ext:browser:keep';
const rule = 'browser.realtime.subscribe.update';
const keepRule = 'browser.realtime.subscribe.keep';
// All IDs/credentials are synthetic, scoped to the launcher's disposable DB.
sql(`INSERT INTO plinth.users(id,username,password_hash) VALUES
    ('${user}','browser-${user}','not-a-password-hash');
    INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES ('${session}','${user}','${hash}');
    INSERT INTO plinth.groups(id,name) VALUES ('${group}','browser-${group}');
    INSERT INTO plinth.group_members(group_id,user_id) VALUES ('${group}','${user}');
    INSERT INTO plinth.rbac_rules(rule,namespace,description,extension_name)
    VALUES ('${rule}','browser','Browser test grant','browser'),
           ('${keepRule}','browser','Browser keepalive grant','browser') ON CONFLICT DO NOTHING;
    INSERT INTO plinth.group_rules(group_id,rule_id)
    SELECT '${group}',id FROM plinth.rbac_rules WHERE rule IN ('${rule}','${keepRule}');`);
assert.equal(sql(`SELECT count(*) FROM plinth.group_members gm
    JOIN plinth.group_rules gr USING(group_id) JOIN plinth.rbac_rules r ON r.id=gr.rule_id
    WHERE gm.user_id='${user}' AND r.rule='kernel.admin'`), '0');

function publish(value) {
    const envelope = JSON.stringify({ layer: 'extension', channel, value });
    sql(`SELECT pg_notify('plinth:realtime', ${literal(envelope)})`);
}
const browser = await chromium.launch({
    executablePath: process.env.PLINTH_BROWSER || undefined,
    args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
});
try {
    const context = await browser.newContext();
    await context.addCookies([{ name: 'plinth_session', value: token,
        url: baseURL, httpOnly: true, sameSite: 'Strict', secure: baseURL.startsWith('https:') }]);
    const page = await context.newPage();
    const connections = [];
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('websocket', socket => {
        const connection = { sent: [], received: [], closed: false };
        connections.push(connection);
        socket.on('framesent', event => {
            try { connection.sent.push(JSON.parse(event.payload)); } catch { /* protocol frames */ }
        });
        socket.on('framereceived', event => {
            try { connection.received.push(JSON.parse(event.payload)); } catch { /* protocol frames */ }
        });
        socket.on('close', () => { connection.closed = true; });
    });
    await page.addInitScript(() => {
        const BrowserWebSocket = window.WebSocket;
        window.__realSockets = [];
        window.WebSocket = class extends BrowserWebSocket {
            constructor(...args) { super(...args); window.__realSockets.push(this); }
        };
    });
    // The snapshot is deterministic; the live transport and PostgreSQL writer,
    // RBAC delivery and heartbeat are entirely the production implementation.
    await page.route('**/api/cap/browser.snapshot', route => route.fulfill({
        json: { ok: true, value: { value: 'snapshot' } },
    }));
    await page.goto(baseURL + '/app/');
    assert.match(await page.evaluate(() => new URL(document.baseURI).pathname),
        /^\/ext\/shell\/[^/]+\/$/, 'production document selects versioned module assets');
    assert.equal(await page.evaluate(() => document.cookie.includes('plinth_session')), false);
    await page.evaluate(async ({ channel, keepChannel }) => {
        const sdk = await import('@plinth/frontend/sdk');
        const { h, render } = await import('preact');
        window.__sdk = sdk;
        window.__events = [];
        window.__subscriptionErrors = [];
        window.__keep = sdk.subscribe(keepChannel, () => {});
        window.__remove = sdk.subscribe(channel, event => window.__events.push(event), {
            onError: error => window.__subscriptionErrors.push(error.code),
        });
        const container = document.createElement('div');
        container.id = 'realtime-hook';
        document.body.append(container);
        function Hook() {
            const { data, error } = sdk.useData(channel, {
                snapshot: { capability: 'browser.snapshot' },
            });
            return h('div', null,
                h('output', { id: 'live-data' }, data?.payload?.value ?? data?.value ?? 'loading'),
                h('output', { id: 'live-error' }, error?.code ?? 'none'));
        }
        render(h(Hook), container);
        window.__unmountHook = () => render(null, container);
    }, { channel, keepChannel });
    await page.getByText('snapshot', { exact: true }).waitFor();
    await page.waitForFunction(() => window.__sdk.getRealtimeState().status === 'connected');
    // Wait for the server acknowledgement before publishing, not a fixed delay.
    async function waitFor(check, message, timeout = 10000) {
        const deadline = Date.now() + timeout;
        while (!check()) {
            if (Date.now() >= deadline) throw new Error(message);
            await new Promise(resolve => setTimeout(resolve, 25));
        }
    }
    const hasGrant = connection => connection.received.some(frame =>
        frame.type === 'subscribed' && frame.channels.includes(channel));
    await waitFor(() => connections.length === 1 && hasGrant(connections[0]), 'no non-admin channel grant');
    publish('live-one');
    await page.getByText('live-one', { exact: true }).waitFor();
    assert.equal(await page.evaluate(() => window.__events.length), 1);
    // Launcher's accelerated heartbeat still traverses the real server timers.
    await waitFor(() => connections[0].received.filter(f => f.type === 'ping').length >= 3,
        'three production heartbeats did not arrive', 10000);
    for (const ping of connections[0].received.filter(f => f.type === 'ping').slice(0, 3)) {
        assert(connections[0].sent.some(f => f.type === 'pong' && f.timestamp === ping.timestamp));
    }
    assert.equal(connections[0].closed, false);
    assert.equal(connections[0].sent.filter(f => f.type === 'subscribe' && f.channels.includes(channel)).length, 1);
    assert.equal(connections[0].sent.some(f => f.type === 'auth'), false);

    // Real transport disconnect: SDK owns reconnect and reasserts only live handlers.
    await page.evaluate(() => window.__realSockets[0].close());
    await page.getByText('disconnected', { exact: true }).waitFor();
    await waitFor(() => connections.length === 2 && hasGrant(connections[1]), 'reconnect did not re-subscribe');
    publish('live-two');
    await page.getByText('live-two', { exact: true }).waitFor();
    assert.equal(await page.evaluate(() => window.__events.length), 2);
    assert.equal(await page.locator('#live-error').textContent(), 'none');
    assert.equal(connections[1].sent.filter(f => f.type === 'subscribe' && f.channels.includes(channel)).length, 1);

    await page.evaluate(() => { window.__remove(); window.__unmountHook(); });
    await waitFor(() => connections[1].received.some(f =>
        f.type === 'unsubscribed' && f.channels.includes(channel)), 'missing unsubscribe acknowledgement');
    publish('after-unsubscribe');
    const pingsBefore = connections[1].received.filter(f => f.type === 'ping').length;
    await waitFor(() => connections[1].received.filter(f => f.type === 'ping').length > pingsBefore,
        'remaining subscription did not retain its connection');
    assert.equal(await page.evaluate(() => window.__events.length), 2);
    assert.equal(connections.length, 2);

    // Expiry is detected on the next real authentication attempt, reported to
    // handlers/state, and never converted to an infinite reconnect loop.
    sql(`UPDATE plinth.sessions SET expires_at=NOW()-INTERVAL '1 second' WHERE id='${session}'`);
    await page.evaluate(() => {
        window.__expired = window.__sdk.subscribe('plinth:ext:browser:update', () => {}, {
            onError: error => window.__subscriptionErrors.push(error.code),
        });
        window.__sdk.reconnectRealtime();
    });
    await page.waitForFunction(() => window.__sdk.getRealtimeState().status === 'failed');
    assert.equal(await page.evaluate(() => window.__sdk.getRealtimeState().error.code), 'auth_failed');
    assert((await page.evaluate(() => window.__subscriptionErrors)).includes('auth_failed'));
    const count = connections.length;
    await page.waitForTimeout(1200); // Exceeds first reconnect interval: assert no retry.
    assert.equal(connections.length, count);
    assert.deepEqual(errors, []);
    await context.close();
    console.log('PASS production non-admin cookie auth, PG event/useData, three heartbeats, reconnect, unsubscribe, expiry');
} finally {
    await browser.close();
}
