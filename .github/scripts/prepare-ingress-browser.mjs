// Ephemeral hosted-runner prerequisite, not a workaround for an application failure.
// Chromium's exact-path userns policy is documented at:
// https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import * as fs from 'node:fs/promises';
import { homedir } from 'node:os';
import { dirname, isAbsolute, normalize, resolve, sep } from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const runFile = promisify(execFile);
const check = condition => assert(condition, 'ingress browser prerequisite rejected');
const safePath = path => typeof path === 'string' && isAbsolute(path) &&
    /^[A-Za-z0-9_/.-]+$/.test(path) && normalize(path) === path && !path.endsWith('/');
const inside = (path, parent) => path.startsWith(parent + sep);
const regular = (stat, uid, mode) => stat.isFile() && !stat.isSymbolicLink() &&
    stat.uid === uid && (stat.mode & 0o777) === mode;
const finiteInode = stat => Number.isSafeInteger(stat.dev) && stat.dev >= 0 &&
    Number.isSafeInteger(stat.ino) && stat.ino > 0;
const sameInode = (stat, identity) => identity && finiteInode(stat) &&
    String(stat.dev) === identity.device && String(stat.ino) === identity.inode;

export function runnerIdentity(env, platform, uid, osRelease) {
    check(platform === 'linux' && Number.isInteger(uid) && uid > 0 &&
        env.GITHUB_ACTIONS === 'true' && env.RUNNER_ENVIRONMENT === 'github-hosted' &&
        env.RUNNER_OS === 'Linux');
    const fields = {};
    for (const line of osRelease.split('\n')) {
        if (!line || line.startsWith('#')) continue;
        const match = /^([A-Z_]+)=(?:"([^"\n]*)"|([^"\n]*))$/.exec(line);
        check(match && !Object.hasOwn(fields, match[1]));
        fields[match[1]] = match[2] ?? match[3];
    }
    check(fields.ID === 'ubuntu' && fields.VERSION_ID === '24.04');
    check(/^[1-9][0-9]{0,19}$/.test(env.GITHUB_RUN_ID ?? '') &&
        /^[1-9][0-9]{0,5}$/.test(env.GITHUB_RUN_ATTEMPT ?? ''));
    check(safePath(env.RUNNER_TEMP));
    const name = `plinth-ingress-browser-${env.GITHUB_RUN_ID}-${env.GITHUB_RUN_ATTEMPT}`;
    const directory = `${env.RUNNER_TEMP}/${name}`;
    return { runId: env.GITHUB_RUN_ID, runAttempt: env.GITHUB_RUN_ATTEMPT,
        uid, name, directory, receiptPath: `${directory}/receipt.json`,
        profilePath: `/etc/apparmor.d/${name}`,
        stagingDirectory: `/etc/apparmor.d/.${name}-staging`,
        stagingPolicy: `/etc/apparmor.d/.${name}-staging/policy` };
}

export function namespaceFailure(error) {
    // Only signatures recognized by the pinned Playwright Chromium launcher,
    // or Chromium's explicit namespace EPERM diagnostic, permit a retry.
    return error instanceof Error && (error.message.includes('No usable sandbox!') ||
        error.message.includes('Chromium sandboxing failed!') ||
        /Failed to move to new namespace:[^\n]*Operation not permitted/.test(error.message));
}

export function profileText(identity, executablePath) {
    check(safePath(executablePath));
    return `abi <abi/4.0>,\nprofile ${identity.name} "${executablePath}" flags=(unconfined) {\n  userns,\n}\n`;
}

async function absent(io, path) {
    try { await io.lstat(path); return false; }
    catch (error) { if (error.code === 'ENOENT') return true; throw error; }
}

async function canonicalDirectory(io, path, uid) {
    check(safePath(path) && await io.realpath(path) === path);
    const stat = await io.lstat(path);
    check(stat.isDirectory() && !stat.isSymbolicLink() && stat.uid === uid &&
        (stat.mode & 0o022) === 0);
}

async function scope(context, io) {
    const identity = runnerIdentity(context.env, context.platform, context.uid,
        await io.readFile('/etc/os-release', 'utf8'));
    check(safePath(context.repo) && safePath(context.home) &&
        context.env.GITHUB_WORKSPACE === context.repo &&
        await io.realpath(context.repo) === context.repo &&
        !inside(identity.directory, context.repo));
    await canonicalDirectory(io, context.env.RUNNER_TEMP, context.uid);
    await canonicalDirectory(io, context.home, context.uid);
    await canonicalDirectory(io, '/etc/apparmor.d', 0);
    return identity;
}

async function executable(context, io, path) {
    const cache = `${context.home}/.cache/ms-playwright`;
    check(!context.env.PLAYWRIGHT_BROWSERS_PATH && safePath(path) && inside(path, cache) &&
        /^chromium-[1-9][0-9]*\/chrome-linux64\/chrome$/.test(path.slice(cache.length + 1)));
    await canonicalDirectory(io, cache, context.uid);
    check(await io.realpath(path) === path);
    const stat = await io.lstat(path);
    check(stat.isFile() && !stat.isSymbolicLink() && stat.uid === context.uid &&
        (stat.mode & 0o111) !== 0 && (stat.mode & 0o022) === 0);
}

async function exportReceipt(context, io, identity) {
    const path = context.env.GITHUB_ENV;
    check(safePath(path) && inside(path, context.env.RUNNER_TEMP) &&
        !inside(path, identity.directory) && await io.realpath(path) === path);
    const stat = await io.lstat(path);
    check(stat.isFile() && !stat.isSymbolicLink() && stat.uid === context.uid &&
        (stat.mode & 0o022) === 0);
    await io.appendFile(path, `PLINTH_INGRESS_BROWSER_RECEIPT=${identity.receiptPath}\n`);
}

async function loaded(io, identity) {
    const result = await io.command('/usr/bin/sudo', ['-n', '/usr/bin/cat',
        '/sys/kernel/security/apparmor/profiles']);
    return result.stdout.split('\n').some(line => line.startsWith(identity.name + ' ('));
}

export async function cleanup(context, io) {
    const identity = await scope(context, io);
    if (await absent(io, identity.directory)) {
        check(await absent(io, identity.profilePath) &&
            await absent(io, identity.stagingDirectory) && !await loaded(io, identity));
        return;
    }
    await canonicalDirectory(io, identity.directory, context.uid);
    const receiptStat = await io.lstat(identity.receiptPath);
    check(regular(receiptStat, context.uid, 0o600));
    const receipt = JSON.parse(await io.readFile(identity.receiptPath, 'utf8'));
    check(Object.keys(receipt).sort().join(',') === 'executablePath,profileAttempted,profileIdentity,runAttempt,runId,schema,stagingIdentity' &&
        receipt.schema === 'plinth.ingress-sandbox.v1' &&
        receipt.runId === identity.runId && receipt.runAttempt === identity.runAttempt &&
        typeof receipt.profileAttempted === 'boolean');
    for (const witness of [receipt.profileIdentity, receipt.stagingIdentity]) {
        check(witness === null || (receipt.profileAttempted &&
            Object.keys(witness).sort().join(',') === 'device,inode' &&
            /^[0-9]+$/.test(witness.device) && /^[1-9][0-9]*$/.test(witness.inode)));
    }
    await executable(context, io, receipt.executablePath);
    const policy = profileText(identity, receipt.executablePath);
    if (!await absent(io, identity.stagingDirectory)) {
        await canonicalDirectory(io, identity.stagingDirectory, 0);
        const stat = await io.lstat(identity.stagingDirectory);
        check((stat.mode & 0o777) === 0o755 && sameInode(stat, receipt.stagingIdentity));
        if (!await absent(io, identity.stagingPolicy)) {
            const staged = await io.lstat(identity.stagingPolicy);
            check(regular(staged, 0, 0o644) && await io.readFile(identity.stagingPolicy, 'utf8') === policy &&
                (receipt.profileIdentity === null || sameInode(staged, receipt.profileIdentity)));
        }
    }
    if (!await absent(io, identity.profilePath)) {
        const stat = await io.lstat(identity.profilePath);
        check(receipt.profileAttempted && sameInode(stat, receipt.profileIdentity) &&
            regular(stat, 0, 0o644) &&
            await io.readFile(identity.profilePath, 'utf8') === policy);
        if (await loaded(io, identity)) {
            await io.command('/usr/bin/sudo', ['-n', '/usr/sbin/apparmor_parser',
                '--remove', identity.profilePath]);
            check(!await loaded(io, identity));
        }
        await io.command('/usr/bin/sudo', ['-n', '/usr/bin/unlink', identity.profilePath]);
        check(await absent(io, identity.profilePath));
    } else {
        check(!await loaded(io, identity));
    }
    if (!await absent(io, identity.stagingDirectory)) {
        if (!await absent(io, identity.stagingPolicy)) {
            await io.command('/usr/bin/sudo', ['-n', '/usr/bin/unlink', identity.stagingPolicy]);
        }
        await io.command('/usr/bin/sudo', ['-n', '/usr/bin/rmdir', '--', identity.stagingDirectory]);
        check(await absent(io, identity.stagingDirectory));
    }
    const source = `${identity.directory}/policy`;
    if (!await absent(io, source)) {
        check(regular(await io.lstat(source), context.uid, 0o600) &&
            await io.readFile(source, 'utf8') === policy);
        await io.unlink(source);
    }
    await io.unlink(identity.receiptPath);
    await io.rmdir(identity.directory); // Refuse unexpected members; never recursive.
}

export async function prepare(context, io) {
    const identity = await scope(context, io);
    check(context.playwrightVersion === '1.63.0');
    await executable(context, io, context.executablePath);
    check(await absent(io, identity.directory) && await absent(io, identity.profilePath) &&
        await absent(io, identity.stagingDirectory) &&
        !await loaded(io, identity));
    await io.mkdir(identity.directory, { mode: 0o700 });
    const freshDirectory = await io.lstat(identity.directory);
    check(finiteInode(freshDirectory));
    const receipt = { schema: 'plinth.ingress-sandbox.v1',
        runId: identity.runId, runAttempt: identity.runAttempt,
        executablePath: context.executablePath, profileAttempted: false,
        profileIdentity: null, stagingIdentity: null };
    try {
        await io.writeFile(identity.receiptPath, JSON.stringify(receipt) + '\n', { flag: 'wx', mode: 0o600 });
    } catch (error) {
        // No privileged mutation has started. Remove only this freshly created
        // directory when its inode is still ours and it is empty. A partial or
        // conflicting receipt remains red, rather than adopting unknown bytes.
        if (sameInode(await io.lstat(identity.directory), {
            device: String(freshDirectory.dev), inode: String(freshDirectory.ino) }) &&
            await absent(io, identity.receiptPath)) await io.rmdir(identity.directory);
        throw error;
    }
    try {
        await exportReceipt(context, io, identity);
        try {
            await io.probe(context.executablePath);
            return;
        } catch (error) {
            check(namespaceFailure(error) &&
                (await io.readFile('/proc/sys/kernel/apparmor_restrict_unprivileged_userns', 'utf8')).trim() === '1');
        }
        io.announce?.('ingress browser sandbox: namespace profile required');
        const policy = profileText(identity, context.executablePath);
        const source = `${identity.directory}/policy`;
        const installed = identity.stagingPolicy;
        await io.writeFile(source, policy, { flag: 'wx', mode: 0o600 });
        check(await absent(io, identity.stagingDirectory) && await absent(io, identity.profilePath));
        receipt.profileAttempted = true;
        await io.writeFile(identity.receiptPath, JSON.stringify(receipt) + '\n', { flag: 'w', mode: 0o600 });
        // mkdir is exclusive, and the root-owned directory cannot be populated
        // by the non-root runner. Staging here also removes an EXDEV assumption.
        await io.command('/usr/bin/sudo', ['-n', '/usr/bin/mkdir', '--mode=0755', '--', identity.stagingDirectory]);
        await canonicalDirectory(io, identity.stagingDirectory, 0);
        const stagingStat = await io.lstat(identity.stagingDirectory);
        check(finiteInode(stagingStat) && (stagingStat.mode & 0o777) === 0o755);
        receipt.stagingIdentity = { device: String(stagingStat.dev), inode: String(stagingStat.ino) };
        await io.writeFile(identity.receiptPath, JSON.stringify(receipt) + '\n', { flag: 'w', mode: 0o600 });
        check(await absent(io, installed));
        await io.command('/usr/bin/sudo', ['-n', '/usr/bin/install', '--owner=0', '--group=0',
            '--mode=0644', '--no-target-directory', '--', source, installed]);
        const installedStat = await io.lstat(installed);
        check(finiteInode(installedStat) && regular(installedStat, 0, 0o644) &&
            await io.readFile(installed, 'utf8') === policy);
        receipt.profileIdentity = { device: String(installedStat.dev), inode: String(installedStat.ino) };
        await io.writeFile(identity.receiptPath, JSON.stringify(receipt) + '\n', { flag: 'w', mode: 0o600 });
        // Atomic link creation cannot overwrite a raced existing or symlink profile.
        await io.command('/usr/bin/sudo', ['-n', '/usr/bin/ln', '--', installed, identity.profilePath]);
        check(sameInode(await io.lstat(identity.profilePath), receipt.profileIdentity) &&
            regular(await io.lstat(identity.profilePath), 0, 0o644) &&
            await io.readFile(identity.profilePath, 'utf8') === policy);
        await io.command('/usr/bin/sudo', ['-n', '/usr/sbin/apparmor_parser',
            '--add', identity.profilePath]);
        check(await loaded(io, identity));
        await io.probe(context.executablePath);
    } catch {
        try { await cleanup(context, io); } catch { /* Always red; CI repeats exact-owned cleanup. */ }
        throw new Error('ingress browser prerequisite failed');
    }
}

async function bounded(promise, timeout = 5000) {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('ingress browser probe deadline')), timeout);
        })]);
    } finally { clearTimeout(timer); }
}

export async function probe(chromium, executablePath) {
    let browser, context;
    try {
        browser = await chromium.launch({ executablePath, chromiumSandbox: true, timeout: 15000,
            args: ['--disable-background-networking', '--disable-component-update',
                '--disable-sync', '--no-first-run', '--metrics-recording-only',
                '--host-resolver-rules=MAP * ~NOTFOUND',
                '--disable-features=MediaRouter,OptimizationHints'] });
        context = await bounded(browser.newContext({ serviceWorkers: 'block', offline: true }));
        await bounded(context.route('**/*', route => route.abort('blockedbyclient')));
        const page = await bounded(context.newPage());
        await page.goto('about:blank', { timeout: 5000 });
    } finally {
        // Playwright's launch timeout owns failed launches. The workflow's
        // child-subreaper additionally owns descendants of this entire helper.
        const closes = await Promise.allSettled([
            ...(context ? [bounded(Promise.resolve().then(() => context.close()))] : []),
            ...(browser ? [bounded(Promise.resolve().then(() => browser.close()))] : []),
        ]);
        check(closes.every(result => result.status === 'fulfilled'));
    }
}

async function main() {
    delete process.env.DEBUG;
    delete process.env.PWDEBUG;
    const context = { env: process.env, platform: process.platform,
        uid: process.getuid?.(), home: homedir(), repo };
    const io = { ...fs, announce: message => console.log(message),
        command: (file, args) => runFile(file, args, {
        timeout: 15000, maxBuffer: 2 * 1024 * 1024, encoding: 'utf8' }) };
    // Validate the host before resolving dependencies or attempting any launch.
    await scope(context, io);
    check(process.argv.length === 2 ||
        (process.argv.length === 3 && process.argv[2] === '--cleanup'));
    if (process.argv[2] === '--cleanup') {
        await cleanup(context, io);
    } else {
        const require = createRequire(import.meta.url);
        const module = `${repo}/tests/browser/node_modules/playwright`;
        context.playwrightVersion = require(`${module}/package.json`).version;
        const { chromium } = require(module);
        context.executablePath = chromium.executablePath();
        io.probe = path => probe(chromium, path);
        await prepare(context, io);
    }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    // The workflow wraps this helper in the Linux child-subreaper. A bounded
    // hard exit lets that owner reap a transport that ignored close/timeout.
    const watchdog = setTimeout(() => {
        console.error('ingress browser sandbox: failed');
        process.exit(1);
    }, 120000);
    try {
        await main();
        console.log(process.argv[2] === '--cleanup' ?
            'ingress browser sandbox: cleaned' : 'ingress browser sandbox: ready');
    } catch {
        console.error('ingress browser sandbox: failed');
        process.exitCode = 1;
        setTimeout(() => process.exit(1), 100).unref();
    } finally {
        clearTimeout(watchdog);
    }
}
