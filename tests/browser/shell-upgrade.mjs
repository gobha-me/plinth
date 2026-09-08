import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
assert(baseURL, 'run through run-production.py --upgrade-cache');
const profile = process.env.PLINTH_BROWSER_PROFILE_DIR ||
    await mkdtemp(join(tmpdir(), 'plinth-upgrade-profile-'));
const commands = createInterface({ input: process.stdin });
const iterator = commands[Symbol.asyncIterator]();
let context;
try {
    // Routing interceptors disable Chromium's HTTP cache. This regression uses
    // neither routes nor cache overrides, and keeps one persistent profile.
    context = await chromium.launchPersistentContext(profile, {
        executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    });
    const page = await context.newPage();
    const failures = [];
    page.on('pageerror', error => failures.push(error.message));
    page.on('requestfailed', request => failures.push(`${request.url()}: ${request.failure().errorText}`));
    const responses = [];
    page.on('response', response => {
        if (['script', 'stylesheet', 'font'].includes(response.request().resourceType())) {
            responses.push(response);
        }
    });
    for (const version of ['901.0.1', '901.0.2']) {
        responses.length = 0;
        const sessionResponse = page.waitForResponse(response =>
            new URL(response.url()).pathname === '/api/auth/session');
        const documentResponse = await page.goto(baseURL + '/app/');
        assert.equal(documentResponse.headers()['cache-control'], 'no-cache');
        // The sign-in heading can render before the session lookup finishes.
        // Finish that owned request before later navigating away from the page.
        assert.equal(await (await sessionResponse).finished(), null);
        try {
            await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
        } catch (cause) {
            throw new Error(`Package ${version} failed to start: ${JSON.stringify(failures)}`, { cause });
        }
        assert.equal(await page.locator('meta[name="plinth-cache-version"]').getAttribute('content'), version);
        const executed = await page.evaluate(() => globalThis.__plinthCacheVersions);
        assert(executed?.length >= 5, 'packaged startup module graph must execute');
        for (const [module, observed] of executed) {
            assert.equal(observed, version, `${module} ran stale or mixed package code`);
        }
        if (version === '901.0.1') {
            assert.equal(await page.locator('base').count(), 0, 'warm-up must use the historical document');
        } else {
            assert.equal(await page.locator('base').getAttribute('href'), `/ext/shell/${version}/`);
        }
        for (const required of ['shell.js', 'sdk.js', 'prepaint.js', 'vendor/preact.module.js']) {
            assert(executed.some(([module]) => module === required), `${required} did not execute`);
        }
        assert.equal(await page.evaluate(() => getComputedStyle(document.documentElement)
            .getPropertyValue('--plinth-cache-version').trim()), version);
        const assetPrefix = version === '901.0.1' ? '/app/' : `/ext/shell/${version}/`;
        assert(responses.some(response => new URL(response.url()).pathname === assetPrefix + 'css/tokens.css'));
        for (const response of responses) {
            assert(response.status() < 400, `${response.status()} ${response.url()}`);
            const path = new URL(response.url()).pathname;
            if (path.startsWith(assetPrefix)) {
                assert.equal(response.headers()['cache-control'], 'public, max-age=31536000, immutable', response.url());
            }
            if (version === '901.0.2') {
                assert(!path.startsWith('/app/'), `replacement requested a mutable asset: ${path}`);
            }
        }
        const versioned = await context.request.get(`${baseURL}/ext/shell/${version}/sdk.js`);
        assert(versioned.ok());
        assert.equal(versioned.headers()['cache-control'], 'public, max-age=31536000, immutable');
        assert.deepEqual(failures, []);
        if (version === '901.0.1') {
            // Ordinary navigation destroys the old document but preserves disk
            // cache. No hard reload, cache clearing, request routing or new profile.
            await page.goto('about:blank');
            console.log('ready_for_upgrade');
            const command = await iterator.next();
            assert.equal(command.value, 'continue');
        }
    }
    console.log('PASS legacy immutable cache -> versioned replacement: HTML, startup JS graph, CSS; one persistent profile');
} finally {
    commands.close();
    await context?.close();
    await rm(profile, { recursive: true, force: true });
}
