// Controlled production-browser regression for SDK owner admission. Requires
// the runner's disposable database; tokens and raw frames are never printed.
// This checks retirement/admission policy, not removal of another fixture's
// Home barrier or unique causation of an earlier CI failure.
import assert from 'node:assert/strict';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
assert(baseURL, 'PLINTH_BASE_URL must name a task-owned production kernel');
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env['PLINTH_PG_' + suffix], 'owned database environment required');
    pgEnv['PG' + suffix] = process.env['PLINTH_PG_' + suffix];
}
const user = randomUUID(), group = randomUUID(), session = randomUUID();
const token = randomBytes(32).toString('base64url');
const hash = createHash('sha256').update(token).digest('hex');
const channel = 'plinth:data:ext_shell.user_preferences';
try {
    execFileSync('psql', ['-X', '-v', 'ON_ERROR_STOP=1', '-c', `
        INSERT INTO plinth.users(id,username,password_hash)
            VALUES('${user}','readiness-${user}','not-a-password-hash');
        INSERT INTO plinth.sessions(id,user_id,token_hash)
            VALUES('${session}','${user}','${hash}');
        INSERT INTO plinth.groups(id,name) VALUES('${group}','readiness-${group}');
        INSERT INTO plinth.group_members(group_id,user_id) VALUES('${group}','${user}');
        INSERT INTO plinth.rbac_rules(rule,namespace,description,extension_name)
            VALUES('shell.realtime.subscribe','shell','Readiness diagnostic','shell')
            ON CONFLICT DO NOTHING;
        INSERT INTO plinth.group_rules(group_id,rule_id)
            SELECT '${group}',id FROM plinth.rbac_rules WHERE rule='shell.realtime.subscribe';
    `], { env: pgEnv, timeout: 15000, stdio: ['ignore', 'ignore', 'pipe'] });
} catch { throw new Error('owned fake identity setup failed'); }

const browser = await chromium.launch({
    executablePath: process.env.PLINTH_BROWSER || undefined,
    args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
});
try {
    const context = await browser.newContext();
    await context.addCookies([{ name: 'plinth_session', value: token, url: baseURL,
        httpOnly: true, sameSite: 'Strict', secure: baseURL.startsWith('https:') }]);
    const page = await context.newPage();
    let releaseAuth;
    const authGate = new Promise(resolve => { releaseAuth = resolve; });
    let authRequested;
    const authSeen = new Promise(resolve => { authRequested = resolve; });
    await page.route('**/api/auth/session', async route => {
        const response = await route.fetch();
        assert.equal(response.status(), 200, 'real owned session response is authenticated');
        authRequested();
        await authGate;
        await route.fulfill({ response });
    });
    await page.addInitScript(({ channel }) => {
        const NativeWebSocket = window.WebSocket;
        window.__heldAck = null;
        window.__ackHeld = false;
        window.__holdFirstAck = true;
        window.__oldSocketClosed = false;
        window.WebSocket = class extends NativeWebSocket {
            addEventListener(type, listener, options) {
                if (type !== 'message') return super.addEventListener(type, listener, options);
                return super.addEventListener(type, event => {
                    let frame;
                    try { frame = JSON.parse(event.data); } catch { /* pass through */ }
                    if (window.__holdFirstAck && frame?.type === 'subscribed' &&
                        frame.channels?.includes(channel)) {
                        window.__holdFirstAck = false;
                        window.__ackHeld = true;
                        window.__heldAck = () => listener.call(this, event);
                        super.addEventListener('close', () => { window.__oldSocketClosed = true; });
                        return;
                    }
                    listener.call(this, event);
                }, options);
            }
        };
    }, { channel });
    try {
        await page.goto(baseURL + '/app/');
        let gateTimer;
        try {
            await Promise.race([authSeen, new Promise((_, reject) => {
                gateTimer = setTimeout(() => reject(new Error('auth response gate deadline')), 10000);
            })]);
        } finally { clearTimeout(gateTimer); }
        await page.evaluate(async ({ channel }) => {
            const sdk = await import('@plinth/frontend/sdk');
            window.__sdk = sdk;
            window.__oldReady = 0;
            window.__oldErrors = [];
            window.__removeOld = sdk.subscribe(channel, () => {}, {
                onReady: () => { window.__oldReady++; },
                onError: error => window.__oldErrors.push(error.code),
            });
        }, { channel });
        await page.waitForFunction(() => window.__ackHeld);
        assert.deepEqual(await page.evaluate(() => ({ ready: window.__oldReady,
            errors: window.__oldErrors.length })), { ready: 0, errors: 0 });
        releaseAuth();
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
        await page.waitForFunction(() => window.__oldSocketClosed);
        await page.evaluate(() => {
            const held = window.__heldAck;
            window.__heldAck = null;
            held();
        });
        assert.deepEqual(await page.evaluate(() => ({ ready: window.__oldReady,
            errors: window.__oldErrors.length })), { ready: 0, errors: 0 },
        'old subscription was silently retired after actual auth completion');
        await page.evaluate(({ channel }) => {
            window.__freshReady = 0;
            window.__freshErrors = [];
            window.__removeFresh = window.__sdk.subscribe(channel, () => {}, {
                onReady: () => { window.__freshReady++; },
                onError: error => window.__freshErrors.push(error.code),
            });
        }, { channel });
        await page.waitForFunction(() => window.__freshReady || window.__freshErrors.length);
        assert.deepEqual(await page.evaluate(() => ({ ready: window.__freshReady,
            errors: window.__freshErrors.length })), { ready: 1, errors: 0 });
        await page.evaluate(() => { window.__removeOld(); window.__removeFresh(); });
        console.log('PASS controlled old-ordering loss: oldReady=0 oldErrors=0; authenticated admission freshReady=1 freshErrors=0');
    } finally { releaseAuth(); }
    await context.close();
} finally { await browser.close(); }
