import assert from 'node:assert/strict';
import test from 'node:test';
import { cleanup, namespaceFailure, prepare, probe, profileText, runnerIdentity } from
    '../../.github/scripts/prepare-ingress-browser.mjs';

const uid = 1001;
const osRelease = 'ID=ubuntu\nVERSION_ID="24.04"\n';
const executablePath = '/home/runner/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome';
function fixture(probes = [null]) {
    const context = { platform: 'linux', uid, home: '/home/runner', repo: '/work/plinth',
        playwrightVersion: '1.63.0', executablePath,
        env: { GITHUB_ACTIONS: 'true', RUNNER_ENVIRONMENT: 'github-hosted', RUNNER_OS: 'Linux',
            GITHUB_RUN_ID: '123456', GITHUB_RUN_ATTEMPT: '2', RUNNER_TEMP: '/runner/temp',
            GITHUB_WORKSPACE: '/work/plinth', GITHUB_ENV: '/runner/temp/env' } };
    const identity = runnerIdentity(context.env, context.platform, uid, osRelease);
    const files = new Map();
    let inode = 100;
    const add = (path, text = '', owner = uid, mode = 0o600, type = 'file') =>
        files.set(path, { text, uid: owner, mode, type, realpath: path, dev: 1, ino: ++inode });
    for (const path of ['/home/runner', '/runner/temp', '/work/plinth',
        '/home/runner/.cache/ms-playwright']) add(path, '', uid, 0o755, 'directory');
    add('/etc/apparmor.d', '', 0, 0o755, 'directory');
    add('/etc/os-release', osRelease, 0, 0o644);
    add('/proc/sys/kernel/apparmor_restrict_unprivileged_userns', '1\n', 0, 0o644);
    add(context.env.GITHUB_ENV);
    add(executablePath, 'fake Chromium', uid, 0o755);
    const missing = () => Object.assign(new Error('fake missing'), { code: 'ENOENT' });
    const get = path => { if (!files.has(path)) throw missing(); return files.get(path); };
    const commands = [], announcements = [], observedProbes = [], loaded = new Set();
    const io = {
        async lstat(path) {
            const record = get(path);
            return { ...record, isFile: () => record.type === 'file',
                isDirectory: () => record.type === 'directory',
                isSymbolicLink: () => record.type === 'symlink' };
        },
        async realpath(path) { return get(path).realpath; },
        async readFile(path) { return get(path).text; },
        async appendFile(path, text) { get(path).text += text; },
        async writeFile(path, text, options) {
            if (options.flag === 'wx') assert.equal(files.has(path), false);
            else assert.equal(files.has(path), true);
            const previous = files.get(path);
            add(path, text, uid, options.mode);
            files.get(path).dev = get(path.slice(0, path.lastIndexOf('/'))).dev;
            if (previous) files.get(path).ino = previous.ino;
        },
        async mkdir(path, options) {
            assert.equal(files.has(path), false); add(path, '', uid, options.mode, 'directory');
            files.get(path).dev = get(path.slice(0, path.lastIndexOf('/'))).dev;
        },
        async unlink(path) { get(path); files.delete(path); },
        async rmdir(path) {
            assert.equal([...files.keys()].some(key => key.startsWith(path + '/')), false);
            get(path); files.delete(path);
        },
        async probe(path) {
            observedProbes.push(path);
            const error = probes.shift();
            if (error) throw error;
        },
        announce(message) { announcements.push(message); },
        async command(file, args) {
            commands.push([file, ...args]);
            assert.equal(file, '/usr/bin/sudo');
            assert.equal(args[0], '-n');
            const action = args[1];
            if (io.failAction === action) throw new Error('fake command failure with private data');
            if (action === '/usr/bin/cat') {
                assert.deepEqual(args.slice(2), ['/sys/kernel/security/apparmor/profiles']);
                return { stdout: [...loaded].map(name => `${name} (unconfined)`).join('\n') };
            }
            if (action === '/usr/bin/install') {
                assert.deepEqual(args.slice(2, -2), ['--owner=0', '--group=0', '--mode=0644', '--no-target-directory', '--']);
                assert.equal(files.has(args.at(-1)), false);
                add(args.at(-1), get(args.at(-2)).text, 0, 0o644);
                files.get(args.at(-1)).dev = get(identity.stagingDirectory).dev;
            } else if (action === '/usr/bin/mkdir') {
                assert.deepEqual(args.slice(2), ['--mode=0755', '--', identity.stagingDirectory]);
                assert.equal(files.has(identity.stagingDirectory), false);
                add(identity.stagingDirectory, '', 0, 0o755, 'directory');
                files.get(identity.stagingDirectory).dev = get('/etc/apparmor.d').dev;
            } else if (action === '/usr/bin/ln') {
                assert.equal(args.at(-1), identity.profilePath);
                assert.equal(files.has(args.at(-1)), false);
                assert.equal(get(args.at(-2)).dev, get('/etc/apparmor.d').dev);
                files.set(args.at(-1), { ...get(args.at(-2)) });
            } else if (action === '/usr/sbin/apparmor_parser') {
                assert.equal(args.at(-1), identity.profilePath);
                if (args[2] === '--add') loaded.add(identity.name);
                else { assert.equal(args[2], '--remove'); loaded.delete(identity.name); }
            } else if (action === '/usr/bin/unlink') {
                assert.equal([identity.profilePath, identity.stagingPolicy].includes(args[2]), true);
                assert.equal(args.length, 3);
                files.delete(args[2]);
            } else if (action === '/usr/bin/rmdir') {
                assert.deepEqual(args.slice(2), ['--', identity.stagingDirectory]);
                await io.rmdir(identity.stagingDirectory);
            } else assert.fail('unexpected privileged command');
            return { stdout: '' };
        },
    };
    return { context, identity, files, add, io, commands, announcements, observedProbes, loaded };
}
const policyCommands = f => f.commands.filter(argv => argv[2] !== '/usr/bin/cat');

test('successful first sandbox probe needs no policy change and exact cleanup is idempotent', async () => {
    const f = fixture();
    await prepare(f.context, f.io);
    assert.deepEqual(f.observedProbes, [executablePath]);
    assert.deepEqual(policyCommands(f), []);
    assert.deepEqual(f.announcements, []);
    assert.equal(f.files.get(f.context.env.GITHUB_ENV).text,
        `PLINTH_INGRESS_BROWSER_RECEIPT=${f.identity.receiptPath}\n`);
    await cleanup(f.context, f.io);
    await cleanup(f.context, f.io);
    assert.deepEqual(policyCommands(f), []);
    assert.equal(f.files.has(f.identity.directory), false);
    assert.equal(f.files.has(f.identity.stagingDirectory), false);
});

test('proven namespace denial permits exact root-owned profile then sandbox reprobe', async () => {
    const f = fixture([new Error('Chromium sandboxing failed!'), null]);
    await prepare(f.context, f.io);
    assert.deepEqual(f.observedProbes, [executablePath, executablePath]);
    assert.deepEqual(f.announcements, ['ingress browser sandbox: namespace profile required']);
    assert.equal(f.files.get(f.identity.profilePath).uid, 0);
    assert.equal(f.files.get(f.identity.profilePath).mode, 0o644);
    assert.equal(f.files.get(f.identity.profilePath).text, profileText(f.identity, executablePath));
    assert.deepEqual([...f.loaded], [f.identity.name]);
    await cleanup(f.context, f.io);
    assert.equal(f.files.has(f.identity.profilePath), false);
    assert.equal(f.files.has(f.identity.directory), false);
    assert.equal(f.loaded.size, 0);
    assert.equal(f.commands.some(argv => argv.includes('--remove')), true);
});

test('unknown startup errors or unproven restriction never change policy', async () => {
    for (const [error, restriction] of [[new Error('unknown private startup error'), '1'],
        [new Error('No usable sandbox!'), '0'], [new Error('No usable sandbox!'), 'invalid'],
        [new Error('Failed to move to new namespace: unrelated failure'), '1'],
        ['No usable sandbox!', '1']]) {
        const f = fixture([error]);
        f.files.get('/proc/sys/kernel/apparmor_restrict_unprivileged_userns').text = restriction;
        await assert.rejects(prepare(f.context, f.io), /ingress browser prerequisite failed/);
        assert.deepEqual(policyCommands(f), []);
        assert.equal(f.files.has(f.identity.directory), false);
    }
});

test('reprobe or profile load failures remain red and remove only owned policy', async () => {
    for (const failure of ['reprobe', 'load']) {
        const f = fixture([new Error('No usable sandbox!'), new Error('unknown reprobe error')]);
        if (failure === 'load') f.io.failAction = '/usr/sbin/apparmor_parser';
        await assert.rejects(prepare(f.context, f.io));
        assert.equal(f.files.has(f.identity.profilePath), false);
        assert.equal(f.files.has(f.identity.directory), false);
        assert.equal(f.loaded.size, 0);
    }
});

test('root, foreign hosts, unsupported OS, ambiguous IDs and runner paths fail before launch', async () => {
    const mutations = [f => { f.context.uid = 0; }, f => { f.context.platform = 'darwin'; },
        f => { f.context.env.GITHUB_ACTIONS = 'false'; },
        f => { f.context.env.RUNNER_ENVIRONMENT = 'self-hosted'; },
        f => { f.context.env.RUNNER_OS = 'Windows'; },
        f => { f.files.get('/etc/os-release').text = 'ID=debian\nVERSION_ID="24.04"\n'; },
        f => { f.files.get('/etc/os-release').text = 'ID=ubuntu\nVERSION_ID="22.04"\n'; },
        f => { f.files.get('/etc/os-release').text = osRelease + 'ID=ubuntu\n'; },
        f => { f.context.env.GITHUB_WORKSPACE = '/foreign'; },
        f => { f.context.env.RUNNER_TEMP = '/runner/temp/../foreign'; },
        f => { f.context.playwrightVersion = '1.62.0'; }];
    for (const field of ['GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT']) {
        for (const value of ['', '0', '01', '-1', '1\nOTHER=x', '../foreign', '1*', '1'.repeat(21)]) {
            mutations.push(f => { f.context.env[field] = value; });
        }
    }
    for (const mutate of mutations) {
        const f = fixture(); mutate(f);
        await assert.rejects(prepare(f.context, f.io));
        assert.deepEqual(f.observedProbes, []);
        assert.deepEqual(f.commands, []);
    }
});

test('executable metacharacters, foreign cache, aliases and unsafe ownership are rejected', async () => {
    for (const path of ['/usr/bin/chrome', executablePath + '*', executablePath + '\n',
        executablePath.replace('/chrome-linux64/', '/../../'),
        executablePath.replace('chromium-1243', 'chromium-@{HOME}'),
        executablePath.replace('chrome-linux64', 'chrome linux64')]) {
        const f = fixture(); f.context.executablePath = path;
        await assert.rejects(prepare(f.context, f.io));
        assert.deepEqual(f.observedProbes, []);
        assert.deepEqual(f.commands, []);
    }
    for (const mutate of [f => { f.files.get(executablePath).type = 'symlink'; },
        f => { f.files.get(executablePath).realpath = '/foreign/chrome'; },
        f => { f.files.get(executablePath).uid = 0; },
        f => { f.files.get(executablePath).mode = 0o777; },
        f => { f.context.env.PLAYWRIGHT_BROWSERS_PATH = '/foreign'; },
        f => { f.files.get('/runner/temp').realpath = '/foreign'; }]) {
        const f = fixture(); mutate(f);
        await assert.rejects(prepare(f.context, f.io));
        assert.deepEqual(f.commands, []);
    }
});

test('existing profile or directory is never overwritten or adopted', async () => {
    for (const target of ['profilePath', 'directory', 'stagingDirectory']) {
        for (const type of ['file', 'directory', 'symlink']) {
            const f = fixture(); f.add(f.identity[target], 'foreign', 0, 0o644, type);
            await assert.rejects(prepare(f.context, f.io));
            assert.equal(f.files.get(f.identity[target]).text, 'foreign');
            assert.deepEqual(f.commands, []);
        }
    }
});

test('cleanup refuses unrelated receipts, symlinks, altered profiles and unexpected members', async () => {
    const mutations = [f => { f.files.get(f.identity.receiptPath).type = 'symlink'; },
        f => { f.files.get(f.identity.receiptPath).uid = 0; },
        f => { const value = JSON.parse(f.files.get(f.identity.receiptPath).text);
            value.runAttempt = '3'; f.files.get(f.identity.receiptPath).text = JSON.stringify(value); },
        f => { f.files.get(f.identity.profilePath).type = 'symlink'; },
        f => { f.files.get(f.identity.profilePath).text += '# foreign'; }];
    for (const mutate of mutations) {
        const f = fixture([new Error('No usable sandbox!'), null]);
        await prepare(f.context, f.io); mutate(f); f.commands.length = 0;
        await assert.rejects(cleanup(f.context, f.io));
        assert.equal(f.files.has(f.identity.profilePath), true);
        assert.deepEqual(f.commands, []);
    }
    const f = fixture(); await prepare(f.context, f.io);
    f.add(f.identity.directory + '/foreign', 'untouched');
    await assert.rejects(cleanup(f.context, f.io));
    assert.equal(f.files.get(f.identity.directory + '/foreign').text, 'untouched');
});

test('GitHub env export failure performs owned cleanup without a policy change', async () => {
    const f = fixture(); f.files.get(f.context.env.GITHUB_ENV).type = 'symlink';
    await assert.rejects(prepare(f.context, f.io));
    assert.deepEqual(policyCommands(f), []);
    assert.deepEqual(f.observedProbes, []);
    assert.equal(f.files.has(f.identity.directory), false);
});

test('namespace failure classifier is narrow and exact-path policy contains no wildcard', () => {
    assert.equal(namespaceFailure(new Error('Failed to move to new namespace: Operation not permitted')), true);
    assert.equal(namespaceFailure(new Error('timeout starting Chromium')), false);
    const f = fixture();
    assert.equal(profileText(f.identity, executablePath).includes('*'), false);
    assert.equal(profileText(f.identity, executablePath).includes('include <tunables/global>'), false);
    assert.equal(profileText(f.identity, executablePath).includes(`"${executablePath}"`), true);
});

test('orphan loaded exact-run policy is neither adopted during prepare nor green cleanup', async () => {
    const f = fixture(); f.loaded.add(f.identity.name);
    await assert.rejects(prepare(f.context, f.io));
    assert.deepEqual(f.observedProbes, []);
    assert.equal(f.files.has(f.identity.directory), false);
    await assert.rejects(cleanup(f.context, f.io));
    assert.deepEqual(policyCommands(f), []);
    assert.deepEqual([...f.loaded], [f.identity.name]);
});

test('cleanup refuses replacement profile even when root mode and policy bytes match', async () => {
    const f = fixture([new Error('No usable sandbox!'), null]);
    await prepare(f.context, f.io);
    f.add(f.identity.profilePath, profileText(f.identity, executablePath), 0, 0o644);
    f.commands.length = 0;
    await assert.rejects(cleanup(f.context, f.io));
    assert.deepEqual(f.commands, []);
    assert.equal(f.files.has(f.identity.profilePath), true);
});

test('no-policy receipt cannot claim cleanup while an unowned exact-run profile is loaded', async () => {
    const f = fixture(); await prepare(f.context, f.io); f.loaded.add(f.identity.name);
    f.commands.length = 0;
    await assert.rejects(cleanup(f.context, f.io));
    assert.deepEqual(policyCommands(f), []);
    assert.equal(f.files.has(f.identity.receiptPath), true);
    assert.deepEqual([...f.loaded], [f.identity.name]);
});

test('failed new receipt creation removes only the proven fresh empty directory', async () => {
    const f = fixture();
    const write = f.io.writeFile;
    f.io.writeFile = async (path, ...args) => {
        if (path === f.identity.receiptPath) throw new Error('fake receipt write failure');
        return write(path, ...args);
    };
    await assert.rejects(prepare(f.context, f.io));
    assert.equal(f.files.has(f.identity.directory), false);
    assert.deepEqual(policyCommands(f), []);
    assert.deepEqual(f.observedProbes, []);
});

test('unsafe or rounded inode metadata cannot authorize policy ownership', async () => {
    for (const field of ['ino', 'dev']) {
        for (const invalid of [NaN, -1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
            const f = fixture([new Error('No usable sandbox!'), null]);
            const command = f.io.command;
            f.io.command = async (file, args) => {
                const result = await command(file, args);
                if (args[1] === '/usr/bin/install') f.files.get(args.at(-1))[field] = invalid;
                return result;
            };
            await assert.rejects(prepare(f.context, f.io));
            assert.equal(f.files.has(f.identity.profilePath), false);
            assert.equal(f.loaded.size, 0);
        }
    }
});

test('actual probe API keeps exact executable sandboxed, offline and owned closed', async () => {
    for (const failedClose of [false, true]) {
        const calls = [];
        const context = {
            async route(pattern, handler) {
                assert.equal(pattern, '**/*');
                await handler({ async abort(reason) { assert.equal(reason, 'blockedbyclient'); } });
            },
            async newPage() { return { async goto(url, options) {
                assert.equal(url, 'about:blank'); assert.equal(options.timeout, 5000);
            } }; },
            close() { calls.push('contextClosed'); if (failedClose) throw new Error('fake close failed'); },
        };
        const browser = {
            async newContext(options) { assert.deepEqual(options, { serviceWorkers: 'block', offline: true }); return context; },
            async close() { calls.push('browserClosed'); },
        };
        const chromium = { async launch(options) {
            assert.equal(options.executablePath, executablePath);
            assert.equal(options.chromiumSandbox, true);
            assert.equal(options.timeout, 15000);
            assert.equal(options.args.includes('--no-sandbox'), false);
            assert.equal(options.args.includes('--host-resolver-rules=MAP * ~NOTFOUND'), true);
            return browser;
        } };
        if (failedClose) await assert.rejects(probe(chromium, executablePath));
        else await probe(chromium, executablePath);
        assert.deepEqual(calls, ['contextClosed', 'browserClosed']);
    }
});

test('root staging and final profile share a filesystem even when runner temporary files do not', async () => {
    const f = fixture([new Error('No usable sandbox!'), null]);
    f.files.get('/runner/temp').dev = 2;
    await prepare(f.context, f.io);
    assert.equal(f.files.get(f.identity.directory).dev, 2);
    assert.equal(f.files.get(f.identity.stagingDirectory).dev, 1);
    assert.equal(f.files.get(f.identity.stagingPolicy).dev, 1);
    assert.equal(f.files.get(f.identity.profilePath).dev, 1);
    assert.equal(f.files.get(f.identity.stagingPolicy).ino, f.files.get(f.identity.profilePath).ino);
    await cleanup(f.context, f.io);
    assert.equal(f.files.has(f.identity.stagingDirectory), false);
    assert.equal(f.files.has(f.identity.profilePath), false);
    assert.equal(f.loaded.size, 0);
});

test('partial owned root staging is cleaned after install or final-link failure', async () => {
    for (const failed of ['/usr/bin/install', '/usr/bin/ln']) {
        const f = fixture([new Error('No usable sandbox!')]); f.io.failAction = failed;
        await assert.rejects(prepare(f.context, f.io));
        assert.equal(f.files.has(f.identity.stagingDirectory), false);
        assert.equal(f.files.has(f.identity.stagingPolicy), false);
        assert.equal(f.files.has(f.identity.profilePath), false);
        assert.equal(f.files.has(f.identity.directory), false);
        assert.equal(f.loaded.size, 0);
        assert.equal(f.commands.some(argv => argv[2] === '/usr/bin/rmdir' &&
            argv.at(-1) === f.identity.stagingDirectory), true);
    }
});

test('foreign root stage identity, symlink, bytes or additional members are never adopted', async () => {
    for (const mutate of [f => { f.files.get(f.identity.stagingDirectory).type = 'symlink'; },
        f => { f.add(f.identity.stagingDirectory, '', 0, 0o755, 'directory'); },
        f => { f.files.get(f.identity.stagingPolicy).text = 'foreign policy'; },
        f => { f.files.get(f.identity.stagingPolicy).type = 'symlink'; }]) {
        const f = fixture([new Error('No usable sandbox!'), null]);
        await prepare(f.context, f.io); mutate(f); f.commands.length = 0;
        await assert.rejects(cleanup(f.context, f.io));
        assert.deepEqual(f.commands, []);
        assert.equal(f.files.has(f.identity.profilePath), true);
        assert.equal(f.files.has(f.identity.stagingDirectory), true);
    }
    const f = fixture([new Error('No usable sandbox!'), null]); await prepare(f.context, f.io);
    f.add(f.identity.stagingDirectory + '/foreign', 'untouched', 0, 0o644);
    await assert.rejects(cleanup(f.context, f.io));
    assert.equal(f.files.get(f.identity.stagingDirectory + '/foreign').text, 'untouched');
    assert.equal(f.files.has(f.identity.stagingDirectory), true);
    assert.equal(f.files.has(f.identity.receiptPath), true);
});

test('missing receipt cannot authorize adoption or cleanup of an unowned root stage', async () => {
    const f = fixture(); f.add(f.identity.stagingDirectory, '', 0, 0o755, 'directory');
    await assert.rejects(cleanup(f.context, f.io));
    assert.deepEqual(f.commands, []);
    assert.equal(f.files.has(f.identity.stagingDirectory), true);
});
