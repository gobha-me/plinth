#!/usr/bin/env python3
"""Native installed-float journey; fails closed without the real protected seam.

Two actual process starts share one owned disposable database and installed
artifact tree. The policy is reviewed fixture data, installed root-owned under
/run between starts; no runtime approval API or fabricated package row is used.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile

from database_cleanup import drop_database
from process_cleanup import start_browser, stop_browser


EXPECTED_CASES = [
    'real protected policy, captured profile, authorized/denied surface filtering',
    'ordinary caller SDK openFloat, ready ID, native relative/dynamic imports, shared modules',
    'guarded SQL minimal descriptor and reload new instance without unsaved state',
    'separate user save and fresh authenticated-session restore isolation',
    'required transient field stays live-only and is excluded from storage',
    'actual CSRF, generic write/delete guard, denied pinned admission and module access',
    'installed render failure and explicit native-admitted Retry isolate component ownership',
    'fresh capability revocation retires only the affected authenticated user',
    'ordinary disable and enable retire old owners without automatic resurrection',
    'failed migration upgrade preserves the old generation fence without rollback claims',
    'successful replacement recovers through new native bytes without inherited persistence approval',
    'normal authenticated package uninstall after consumer contexts close',
]


def artifact_digest(root):
    """Independent profile1 framing of the actual installed tree, not ZIP bytes."""
    records = []
    content = 0
    metadata = len(b'plinth.restore.artifact\0') + 8
    if root.is_symlink() or not root.is_dir():
        raise RuntimeError('installed fixture root is not an ordinary directory')
    pending = [root]
    while pending:
        # Consume directories incrementally; never materialize/sort an unbounded
        # rglob result before enforcing the finite inventory profile.
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                if len(records) == 4096:
                    raise RuntimeError('installed fixture exceeds artifact record profile')
                path = Path(entry.path)
                relative = path.relative_to(root).as_posix().encode('utf-8')
                components = relative.split(b'/')
                if (len(relative) > 1024 or len(components) > 32 or
                    any(not part or part in (b'.', b'..') or len(part) > 255 for part in components)):
                    raise RuntimeError('installed fixture path outside artifact profile')
                info = entry.stat(follow_symlinks=False)
                mode = stat.S_IMODE(info.st_mode)
                regular = stat.S_ISREG(info.st_mode)
                if mode > 0o777 or not (stat.S_ISDIR(info.st_mode) or regular) or (regular and info.st_nlink != 1):
                    raise RuntimeError('installed fixture contains unsupported artifact node')
                charge = 1 + 4 + len(relative) + 4 + (8 if regular else 0)
                if charge > 1048576 - metadata:
                    raise RuntimeError('installed fixture exceeds artifact metadata profile')
                if regular and (info.st_size < 0 or info.st_size > 52428800 - content):
                    raise RuntimeError('installed fixture exceeds artifact content profile')
                # All profile checks precede file reads and record/pending
                # appends. Read at most the stat size plus one to detect growth
                # without allocating an unbounded replacement file.
                prefix = bytes([2 if regular else 1]) + struct.pack('>I', len(relative)) + relative + struct.pack('>I', mode)
                if regular:
                    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
                    with os.fdopen(descriptor, 'rb') as file:
                        opened = os.fstat(file.fileno())
                        if (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns) != (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns):
                            raise RuntimeError('installed fixture changed before bounded read')
                        data = file.read(info.st_size + 1)
                        after = os.fstat(file.fileno())
                    if len(data) != info.st_size or (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (info.st_size, info.st_mtime_ns, info.st_ctime_ns):
                        raise RuntimeError('installed fixture changed during bounded read')
                    record = prefix + struct.pack('>Q', len(data)) + data
                    content += len(data)
                else:
                    record = prefix
                    pending.append(path)
                metadata += charge
                records.append((relative, record))
    if not records:
        raise RuntimeError('installed fixture has no artifact records')
    digest = hashlib.sha256(b'plinth.restore.artifact\0' + struct.pack('>II', 1, len(records)))
    for _, record in sorted(records, key=lambda item: item[0]):
        digest.update(record)
    return digest.hexdigest()


def redact_logs(contents, env):
    """Consistent private-log redaction across browser and kernel failures."""
    hidden = [env.get('PLINTH_BOOTSTRAP_TOKEN'), env.get('PLINTH_PG_PASSWORD'),
              env.get('PLINTH_FLOAT_PASSWORD')]
    state_path = Path(env['PLINTH_FLOAT_STATE']) if env.get('PLINTH_FLOAT_STATE') else None
    if state_path is not None and state_path.exists():
        hidden.extend(account['password'] for account in
                      json.loads(state_path.read_text())['accounts'].values())
    for secret in hidden:
        if secret:
            contents = contents.replace(secret, '[redacted]')
    return contents


def pack_fixture(source, destination):
    # Include explicit directories and fixed ordinary POSIX modes. Installation
    # remains the kernel's normal API ZIP path, not a filesystem seed.
    with zipfile.ZipFile(destination, 'x', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob('*')):
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                raise RuntimeError('unsupported source fixture node')
            name = path.relative_to(source).as_posix() + ('/' if path.is_dir() else '')
            info = zipfile.ZipInfo(name, (2026, 10, 16, 0, 0, 0))
            info.create_system = 3
            info.external_attr = ((stat.S_IFDIR | 0o755) if path.is_dir() else
                                  (stat.S_IFREG | 0o644)) << 16
            archive.writestr(info, b'' if path.is_dir() else path.read_bytes())


def pack_provider_variants(source, staging, destination):
    """Two fixed owned test packages, never changes to the source/install tree."""
    artifact_digest(source)  # Bound the fixed source before copying it.
    for version in ['0.2.0', '0.3.0']:
        variant = staging / ('floatprovider-' + version)
        shutil.copytree(source, variant)
        manifest_path = variant / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        if manifest['name'] != 'floatprovider' or manifest['version'] != '0.1.0':
            raise RuntimeError('unexpected original provider fixture identity')
        manifest['version'] = version
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        if version == '0.2.0':
            migrations = variant / 'migrations'
            migrations.mkdir()
            (migrations / '001_deliberate_failure.sql').write_text(
                'SELECT 1 / 0;\n', encoding='utf-8')
        else:
            (variant / 'client/panels/label.js').write_text(
                "export const label = 'Captured replacement fixture import 0.3.0';\n",
                encoding='utf-8')
        artifact_digest(variant)
        pack_fixture(variant, destination / f'floatprovider-{version}.zip')


def stop_kernel(child):
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
        child.wait(timeout=55)
    if child.returncode != 0:
        raise RuntimeError(f'installed float kernel shutdown failed: {child.returncode}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--bundle-dir', type=Path,
                        help='exact candidate bundled shell directory; defaults beside binary')
    args = parser.parse_args()
    if os.name != 'posix' or not Path('/proc/self/status').exists():
        raise RuntimeError('protected native journey requires Linux')
    binary = args.binary.resolve(strict=True)
    bundle = (args.bundle_dir or binary.parent / 'share/plinth/bundled').resolve(strict=True)
    repo = Path(__file__).resolve().parents[2]
    pg = os.environ.copy()
    for suffix, default in [('HOST', '127.0.0.1'), ('PORT', '5432'), ('USER', 'plinth'),
                            ('PASSWORD', 'plinth'), ('DATABASE', 'plinth_test')]:
        pg['PG' + suffix] = os.environ.get('PLINTH_PG_' + suffix, default)
    pg['PGCONNECT_TIMEOUT'] = '5'
    database = 'plinth_installed_float_' + uuid.uuid4().hex
    privileged = [] if os.geteuid() == 0 else ['sudo', '-n']
    service_uid = os.geteuid() if os.geteuid() != 0 else 65534
    service_gid = os.getegid() if os.geteuid() != 0 else 65534
    protected = Path('/run') / f'plinth-installed-floats-{os.getpid()}-{secrets.token_hex(8)}'
    protected_files = []
    protected_created = False
    created_database = False
    failures = []
    receipt = None

    def command(*argv):
        subprocess.run([*privileged, *argv], check=True, timeout=20, capture_output=True)

    try:
        subprocess.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-c',
                        f'CREATE DATABASE "{database}"'], env=pg, check=True,
                       timeout=20, capture_output=True)
        created_database = True
        command('mkdir', '-m', '0755', '--', str(protected))
        protected_created = True
        executable = protected / 'plinth'
        command('install', '-o', '0', '-g', '0', '-m', '0755', '--', str(binary), str(executable))
        protected_files.append(executable)
        with tempfile.TemporaryDirectory(prefix='plinth-installed-floats-') as temporary:
            root = Path(temporary)
            root.chmod(0o755)
            children = []
            browser = None
            try:
                data = root / 'data'
                data.mkdir(mode=0o700)
                command('chown', f'{service_uid}:{service_gid}', '--', str(data))
                shutil.copytree(repo / 'migrations', root / 'migrations')
                shutil.copytree(bundle, root / 'bundled')
                # Source/build parents may be private. Relax ONLY these owned
                # immutable copies, never source/build/installed artifact trees.
                for folder in [root / 'migrations', root / 'bundled']:
                    folder.chmod(0o755)
                    for path in folder.rglob('*'):
                        if path.is_symlink():
                            raise RuntimeError('unexpected bundled/migration symlink')
                        path.chmod(0o755 if path.is_dir() else 0o644)
                fixtures = root / 'fixtures'
                fixtures.mkdir()
                for source, name in [('float-caller', 'floatcaller'), ('float-provider', 'floatprovider')]:
                    pack_fixture(repo / 'tests/extensions' / source, fixtures / f'{name}.zip')
                variants = root / 'variant-sources'
                variants.mkdir()
                pack_provider_variants(repo / 'tests/extensions/float-provider', variants, fixtures)
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', 0))
                    port = probe.getsockname()[1]
                config = {'database': {'pool_size': 4}, 'dev_mode': False,
                    'registration': {'mode': 'open', 'max_accounts': 4,
                        'source_attempts': 20, 'subject_attempts': 20,
                        'global_attempts': 100, 'window_seconds': 60},
                    'listen_host': '127.0.0.1', 'listen_port': port,
                    'migrations_dir': str(root / 'migrations'),
                    'shell': {'enabled': True, 'bundle_path': str(root / 'bundled')},
                    'packages': {'data_dir': str(data), 'staging_dir': str(data / 'staging'),
                        'upgrade_drain_timeout_ms': 500}}
                config_path = root / 'config.json'
                env = os.environ.copy()
                # The owned config selects every non-PG server setting. Ambient
                # developer toggles must not silently override this actual gate.
                for key in list(env):
                    if key.startswith('PLINTH_') and not key.startswith('PLINTH_BROWSER'):
                        env.pop(key)
                for suffix in ['HOST', 'PORT', 'USER', 'PASSWORD']:
                    env['PLINTH_PG_' + suffix] = pg['PG' + suffix]
                token = secrets.token_urlsafe(48)
                env.update(PLINTH_PG_DATABASE=database, PLINTH_PG_POOL_SIZE='4',
                    PLINTH_BOOTSTRAP_TOKEN=token, PLINTH_TEST_BOOTSTRAP_TOKEN=token,
                    PLINTH_BASE_URL=f'http://127.0.0.1:{port}',
                    PLINTH_FLOAT_STATE=str(root / 'state.json'),
                    PLINTH_FLOAT_PASSWORD='fake-float46-' + secrets.token_hex(16) + '-password',
                    PLINTH_FLOAT_FIXTURES=str(fixtures))
                drop = ['setpriv', '--no-new-privs']
                if os.geteuid() == 0:
                    drop += [f'--reuid={service_uid}', f'--regid={service_gid}', '--clear-groups',
                             '--inh-caps=-all', '--ambient-caps=-all', '--bounding-set=-all']

                def launch(phase):
                    config_path.write_text(json.dumps(config), encoding='utf-8')
                    config_path.chmod(0o644)
                    with (root / f'kernel-{phase}.log').open('w') as output:
                        child = subprocess.Popen([*drop, str(executable), 'serve', '--config', str(config_path)],
                            cwd=root, env=env, stdin=subprocess.DEVNULL,
                            stdout=output, stderr=subprocess.STDOUT)
                    children.append(child)
                    deadline = time.monotonic() + 30
                    while child.poll() is None and time.monotonic() < deadline:
                        try:
                            with urllib.request.urlopen(env['PLINTH_BASE_URL'] + '/healthz', timeout=1) as response:
                                if response.status == 200:
                                    status = Path(f'/proc/{child.pid}/status').read_text()
                                    identities = {line.split(':')[0]: line.split(':')[1].strip()
                                                  for line in status.splitlines() if ':' in line}
                                    if (identities['Uid'].split() != [str(service_uid)] * 4 or
                                        identities['Gid'].split() != [str(service_gid)] * 4 or
                                        identities['NoNewPrivs'] != '1' or
                                        any(int(identities[name], 16) != 0 for name in
                                            ['CapInh', 'CapPrm', 'CapEff', 'CapAmb'])):
                                        raise RuntimeError('kernel does not have protected service identity')
                                    return child
                        except (urllib.error.URLError, TimeoutError):
                            pass
                        time.sleep(0.05)
                    raise RuntimeError('installed float kernel failed readiness within30s')

                def browser_phase(phase):
                    nonlocal browser
                    browser = start_browser(['node', str(repo / 'tests/browser/installed-float-production.mjs'), phase],
                        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    output, error = browser.communicate(timeout=240)
                    if browser.returncode != 0:
                        # Preserve private redacted output; do not render account
                        # passwords, bootstrap tokens or PG credentials.
                        location = Path(tempfile.mkdtemp(prefix='plinth-float-failure-'))
                        for name, contents in [('browser.log', output + '\n' + error),
                                               ('kernel.log', (root / f'kernel-{phase}.log').read_text(errors='replace'))]:
                            contents = redact_logs(contents, env)
                            path = location / name
                            path.write_text(contents, encoding='utf-8')
                            path.chmod(0o600)
                        raise RuntimeError(f'installed float {phase} failed; private diagnostics {location}')
                    lines = output.splitlines()
                    if len(lines) != 1:
                        raise RuntimeError('browser phase must produce exactly one receipt')
                    result = json.loads(lines[0])
                    if result.get('phase') != phase or result.get('actualAuth') is not True:
                        raise RuntimeError('invalid actual browser phase receipt')
                    return result

                first = launch('install')
                installed = browser_phase('install')
                if installed != {'phase': 'install', 'installed': 2, 'actualAuth': True, 'defaultFixtureGrants': 0}:
                    raise RuntimeError('missing exact ordinary install/no-default-grants receipt')
                stop_kernel(first)
                state = json.loads((root / 'state.json').read_text())
                package = state['packages']['floatprovider']
                artifact = artifact_digest(data / 'extensions' / package['name'] / package['version'])
                if artifact != state['receipt']['artifact_digest']:
                    raise RuntimeError('independent installed-tree digest does not match kernel receipt')
                panels = json.loads((repo / 'tests/extensions/float-provider/panels.json').read_text())
                schema = panels['panels'][0]['context_schema']
                encoded = json.dumps(schema, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
                schema_digest = hashlib.sha256(b'plinth.restore.schema\0' + struct.pack('>I', 1) + encoded).hexdigest()
                policy = {'format_version': 1, 'policy_revision': '1', 'review': {
                    'admin_user_id': state['accounts']['operator']['id'],
                    'reviewed_at': '2026-10-16T00:00:00Z', 'review_ref': 'installed-float-fixture',
                    'evidence_sha256': hashlib.sha256(encoded).hexdigest()}, 'entries': [{
                    'application_id': 'floatprovider', 'panel_id': 'preview', 'capability': 'floatprovider:1:preview',
                    'projection_contract': {'id': 'fixture-record', 'version': 1},
                    'artifact_digest': artifact, 'schema_digest': schema_digest,
                    'fields': [{'key': 'record_id', 'meaning': schema['fields']['record_id']['meaning']}]}]}
                source = root / 'reviewed-policy.json'
                source.write_text(json.dumps(policy), encoding='utf-8')
                source.chmod(0o600)
                policy_path = protected / 'policy.json'
                command('install', '-o', '0', '-g', '0', '-m', '0644', '--', str(source), str(policy_path))
                protected_files.append(policy_path)
                config['packages']['restore_policy_file'] = str(policy_path)
                second = launch('verify')
                receipt = browser_phase('verify')
                if (receipt.get('expectedCases') != len(EXPECTED_CASES) or
                    receipt.get('completed') != EXPECTED_CASES or receipt.get('packageProvenance') != 'user' or
                    receipt.get('protectedPolicy') is not True or receipt.get('normalUninstall') is not True):
                    raise RuntimeError('missing complete actual installed float receipt')
                stop_kernel(second)
                receipt.update(installedArtifactDigest=artifact, schemaDigest=schema_digest)
                receipt['binarySha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
                receipt['bundleArchiveHashes'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sorted((root / 'bundled').glob('*.zip'))}
            except BaseException as error:
                # Startup/digest/verification failures also retain task-owned
                # evidence, not just nonzero browser exits. Never print raw logs.
                try:
                    location = Path(tempfile.mkdtemp(prefix='plinth-float-kernel-failure-'))
                    for source in [root / 'kernel-install.log', root / 'kernel-verify.log']:
                        if source.exists():
                            contents = source.read_text(errors='replace')
                            contents = redact_logs(contents, env if 'env' in locals()
                                else {'PLINTH_PG_PASSWORD': pg['PGPASSWORD']})
                            target = location / source.name
                            target.write_text(contents, encoding='utf-8')
                            target.chmod(0o600)
                    error.add_note(f'private redacted kernel diagnostics: {location}')
                except BaseException as diagnostic_error:
                    error.add_note(f'private diagnostic retention failed: {diagnostic_error}')
                raise
            finally:
                if browser is not None:
                    try:
                        stop_browser(browser)
                    except BaseException as error:
                        failures.append(error)
                for child in children:
                    try:
                        stop_kernel(child)
                    except BaseException as error:
                        failures.append(error)
                        if child.poll() is None:
                            child.kill()
                            child.wait(timeout=5)
    except BaseException as error:
        failures.append(error)
    finally:
        for path in reversed(protected_files):
            try:
                command('unlink', '--', str(path))
            except BaseException as error:
                failures.append(error)
        if protected_created:
            try:
                command('rmdir', '--', str(protected))
            except BaseException as error:
                failures.append(error)
        if created_database:
            try:
                drop_database(database, pg)
            except BaseException as error:
                failures.append(error)
    if failures:
        raise BaseExceptionGroup('installed float journey and owned cleanup failures', failures)
    receipt['cleanup'] = 'PASS native processes/browser descendants/protected files/disposable database'
    print(json.dumps(receipt), flush=True)


if __name__ == '__main__':
    main()
