"""Offline controls for the exact test-only native-journey browser owner."""

from contextlib import ExitStack, contextmanager
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import signal
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import warnings
import zipfile


REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    'fixed_browser_harness', REPO / '.github/scripts/run-installed-floats-fixed-browser.py')
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)
REAL_TEMPORARY_DIRECTORY = tempfile.TemporaryDirectory
REAL_RUN_SUPERVISOR = helper.run_supervisor


def archive_bytes(members=None, compression=zipfile.ZIP_STORED):
    if members is None:
        # Like the actual upstream archive, this intentionally has no explicit
        # root directory entry. Parents must be safely created, not required.
        members = [('chrome-linux64/chrome', b'fake executable bytes', stat.S_IFREG | 0o755),
                   ('chrome-linux64/resources/payload', b'fixture-payload-abcdef', stat.S_IFREG | 0o644)]
    output = io.BytesIO()
    with warnings.catch_warnings(), zipfile.ZipFile(output, 'w') as archive:
        warnings.simplefilter('ignore', UserWarning)  # Deliberate duplicate controls.
        for name, content, mode in members:
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.compress_type = compression
            info.external_attr = (mode << 16) | (0x10 if stat.S_ISDIR(mode) else 0)
            archive.writestr(info, content)
    return output.getvalue()


@contextmanager
def pinned_fixture(data):
    with mock.patch.object(helper, 'ARCHIVE_SIZE', len(data)), \
            mock.patch.object(helper, 'ARCHIVE_SHA256', hashlib.sha256(data).hexdigest()):
        yield


class FakeResponse:
    def __init__(self, data, status=200, headers=None, read_error=None):
        self.status = status
        self.headers = [('Content-Length', str(len(data)))] if headers is None else headers
        self.stream = io.BytesIO(data)
        self.read_error = read_error
        self.read_sizes = []
        self.closed = False

    def getheaders(self):
        return self.headers

    def read(self, size):
        if not isinstance(size, int) or size <= 0 or size > 1024 * 1024:
            raise AssertionError('download must use finite bounded reads')
        self.read_sizes.append(size)
        if self.read_error:
            raise self.read_error
        return self.stream.read(size)

    def close(self):
        self.closed = True


@contextmanager
def fake_http(response):
    connection = mock.Mock()
    connection.getresponse.return_value = response
    with mock.patch.object(helper.http.client, 'HTTPSConnection', return_value=connection) as constructor:
        yield constructor, connection


class FixedBrowserControls(unittest.TestCase):
    def test_cli_help_and_argument_errors_keep_argparse_exit_status(self):
        command = [sys.executable, str(REPO / '.github/scripts/run-installed-floats-fixed-browser.py')]
        help_result = subprocess.run([*command, '--help'], capture_output=True, timeout=5)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn(b'--binary', help_result.stdout)
        self.assertEqual(help_result.stderr, b'')
        invalid = subprocess.run(command, capture_output=True, timeout=5)
        self.assertEqual(invalid.returncode, 2)
        self.assertIn(b'--binary', invalid.stderr)
        self.assertNotIn(b'fixed browser preparation', invalid.stderr)

    def test_exact_reviewed_pin_and_finite_transport_profile(self):
        self.assertEqual(helper.BROWSER_VERSION, '157.0.8103.6')
        self.assertEqual(helper.BROWSER_URL,
            'https://storage.googleapis.com/chrome-for-testing-public/157.0.8103.6/linux64/chrome-linux64.zip')
        self.assertEqual(helper.ARCHIVE_SIZE, 202676171)
        self.assertEqual(helper.ARCHIVE_SHA256,
            '7802b25411aa5b8aa387cc733a45df6eed1aea8cc046fd4e2dec64d5918b1853')
        self.assertEqual(helper.EXPECTED_VERSION, b'Google Chrome for Testing 157.0.8103.6 \n')
        self.assertEqual(helper.MAX_ARCHIVE_BYTES, 256 * 1024 * 1024)
        self.assertEqual(helper.MAX_ENTRIES, 512)
        self.assertEqual(helper.MAX_EXPANDED_BYTES, 512 * 1024 * 1024)
        self.assertEqual(helper.MAX_MEMBER_BYTES, 320 * 1024 * 1024)
        self.assertEqual(helper.MAX_PATH_BYTES, 256)
        self.assertEqual(helper.MAX_DEPTH, 8)

    def test_real_stored_and_deflated_archives_create_safe_implicit_parents(self):
        for compression in [zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED]:
            with self.subTest(compression=compression), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                data = archive_bytes(compression=compression)
                source = root / 'browser.zip'
                source.write_bytes(data)
                with pinned_fixture(data):
                    executable = helper.extract_archive(source, root / 'extracted')
                self.assertEqual(executable, root / 'extracted/chrome-linux64/chrome')
                self.assertEqual(executable.read_bytes(), b'fake executable bytes')
                self.assertTrue(executable.stat().st_mode & stat.S_IXUSR)
                self.assertEqual((executable.parent / 'resources/payload').read_bytes(),
                                 b'fixture-payload-abcdef')

    def test_size_and_checksum_are_verified_before_zip_parsing(self):
        data = archive_bytes()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'browser.zip'
            source.write_bytes(data)
            for changes in [{'ARCHIVE_SIZE': len(data) + 1}, {'ARCHIVE_SHA256': '0' * 64},
                            {'MAX_ARCHIVE_BYTES': len(data) - 1}]:
                with self.subTest(changes=changes), pinned_fixture(data), ExitStack() as stack:
                    for name, value in changes.items():
                        stack.enter_context(mock.patch.object(helper, name, value))
                    parse = stack.enter_context(mock.patch.object(helper.zipfile, 'ZipFile'))
                    with self.assertRaises(helper.FixedBrowserError):
                        helper.extract_archive(source, root / 'extracted')
                    parse.assert_not_called()

    def test_unsafe_zip_nodes_names_duplicates_and_prefix_conflicts_refuse(self):
        regular = stat.S_IFREG | 0o644
        executable = ('chrome-linux64/chrome', b'fake executable bytes', stat.S_IFREG | 0o755)
        malformed = [
            [('/absolute', b'x', regular)],
            [('C:/chrome-linux64/escape', b'x', regular)],
            [('chrome-linux64/../escape', b'x', regular)],
            [('chrome-linux64/./escape', b'x', regular)],
            [('chrome-linux64//escape', b'x', regular)],
            [('chrome-linux64\\escape', b'x', regular)],
            [('other-root/escape', b'x', regular)],
            [('chrome-linux64/link', b'../../outside', stat.S_IFLNK | 0o777)],
            [('chrome-linux64/pipe', b'', stat.S_IFIFO | 0o600)],
            [('chrome-linux64/device', b'', stat.S_IFCHR | 0o600)],
            [('chrome-linux64/socket', b'', stat.S_IFSOCK | 0o600)],
            [('chrome-linux64/privileged', b'x', stat.S_IFREG | 0o4755)],
            [executable],  # Duplicate with the mandatory member below.
            [('chrome-linux64/assets', b'x', regular), ('chrome-linux64/assets/child', b'y', regular)],
            [('chrome-linux64/assets/child', b'y', regular), ('chrome-linux64/assets', b'x', regular)],
        ]
        for members in malformed:
            with self.subTest(names=[entry[0] for entry in members]), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                data = archive_bytes([executable, *members])
                source = root / 'browser.zip'
                source.write_bytes(data)
                with pinned_fixture(data), self.assertRaises(helper.FixedBrowserError):
                    helper.extract_archive(source, root / 'extracted')
                self.assertFalse((root / 'escape').exists())
                self.assertFalse((root / 'outside').exists())
                self.assertFalse(any(path.is_file() for path in (root / 'extracted').rglob('*')),
                                 'unsafe metadata must refuse before materialization')

    def test_encrypted_and_unsupported_compression_refuse(self):
        encrypted = bytearray(archive_bytes())
        for marker, offset in [(b'PK\x03\x04', 6), (b'PK\x01\x02', 8)]:
            position = encrypted.index(marker) + offset
            struct.pack_into('<H', encrypted, position, struct.unpack_from('<H', encrypted, position)[0] | 1)
        for data in [bytes(encrypted), archive_bytes(compression=zipfile.ZIP_BZIP2)]:
            with self.subTest(encrypted=data == bytes(encrypted)), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / 'browser.zip'
                source.write_bytes(data)
                with pinned_fixture(data), self.assertRaises(helper.FixedBrowserError):
                    helper.extract_archive(source, root / 'extracted')

    def test_every_inventory_limit_refuses_before_member_open(self):
        data = archive_bytes()
        controls = [('MAX_ENTRIES', 1), ('MAX_EXPANDED_BYTES', 1), ('MAX_MEMBER_BYTES', 1),
                    ('MAX_PATH_BYTES', 1), ('MAX_DEPTH', 1)]
        for name, limit in controls:
            with self.subTest(limit=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / 'browser.zip'
                source.write_bytes(data)
                with pinned_fixture(data), mock.patch.object(helper, name, limit), \
                        mock.patch.object(helper.zipfile.ZipFile, 'open') as opened:
                    with self.assertRaises(helper.FixedBrowserError):
                        helper.extract_archive(source, root / 'extracted')
                    opened.assert_not_called()

    def test_crc_corruption_is_not_hidden_by_the_matching_archive_digest(self):
        data = archive_bytes().replace(b'fixture-payload-abcdef', b'fixture-payload-abcdeg', 1)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'browser.zip'
            source.write_bytes(data)
            with pinned_fixture(data), self.assertRaises(helper.FixedBrowserError):
                helper.extract_archive(source, root / 'extracted')

    def test_missing_executable_and_preexisting_destination_refuse(self):
        data = archive_bytes([('chrome-linux64/resources/payload', b'x', stat.S_IFREG | 0o644)])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'browser.zip'
            source.write_bytes(data)
            with pinned_fixture(data), self.assertRaises(helper.FixedBrowserError):
                helper.extract_archive(source, root / 'missing')
            target = root / 'existing'
            target.mkdir()
            owned = target / 'do-not-overwrite'
            owned.write_bytes(b'preserve existing owner')
            data = archive_bytes()
            source.write_bytes(data)
            with pinned_fixture(data), self.assertRaises(helper.FixedBrowserError):
                helper.extract_archive(source, target)
            self.assertEqual(owned.read_bytes(), b'preserve existing owner')

    def test_exact_https_download_hashes_all_bytes_and_closes_owned_transport(self):
        data = archive_bytes()
        response = FakeResponse(data)
        with tempfile.TemporaryDirectory() as temporary, pinned_fixture(data), fake_http(response) as (constructor, connection):
            target = Path(temporary) / 'download.zip'
            with mock.patch.dict(os.environ, {'HTTPS_PROXY': 'http://fake-proxy.invalid:9999'}):
                helper.download_archive(target)
            self.assertEqual(target.read_bytes(), data)
            self.assertTrue(response.read_sizes)
            self.assertEqual(constructor.call_args.args[0], 'storage.googleapis.com')
            self.assertGreater(constructor.call_args.kwargs['timeout'], 0)
            self.assertLessEqual(constructor.call_args.kwargs['timeout'], 15)
            self.assertEqual(connection.request.call_count, 1)
            args = connection.request.call_args.args
            self.assertEqual(args[:2], ('GET',
                '/chrome-for-testing-public/157.0.8103.6/linux64/chrome-linux64.zip'))
            connection.close.assert_called_once()
            self.assertTrue(response.closed)

    def test_download_errors_never_redirect_fallback_or_accept_changed_bytes(self):
        data = archive_bytes()
        controls = [
            FakeResponse(data, status=302, headers=[('Location', 'https://fake.invalid/browser.zip')]),
            FakeResponse(data, status=500),
            FakeResponse(data, headers=[('Content-Length', '-1')]),
            FakeResponse(data, headers=[('Content-Length', str(len(data))), ('Content-Length', str(len(data)))]),
            FakeResponse(data, headers=[('Content-Length', str(len(data) + 1))]),
            FakeResponse(data[:-1], headers=[('Content-Length', str(len(data)))]),
            FakeResponse(data + b'x', headers=[('Content-Length', str(len(data)))]),
            FakeResponse(data[:-1] + bytes([data[-1] ^ 1])),
            FakeResponse(data, read_error=TimeoutError('fake bounded read timeout')),
            FakeResponse(data, read_error=OSError('fake connection failure')),
        ]
        for index, response in enumerate(controls):
            with self.subTest(control=index), tempfile.TemporaryDirectory() as temporary, pinned_fixture(data), \
                    fake_http(response) as (constructor, connection):
                with self.assertRaises(helper.FixedBrowserError):
                    helper.download_archive(Path(temporary) / 'download.zip')
                self.assertEqual(constructor.call_count, 1, 'never refetch or follow another browser URL')
                connection.close.assert_called_once()
                self.assertTrue(response.closed)

    def test_download_oversize_refuses_before_read_and_connection_failure_propagates(self):
        data = archive_bytes()
        response = FakeResponse(data)
        with tempfile.TemporaryDirectory() as temporary, pinned_fixture(data), \
                mock.patch.object(helper, 'MAX_ARCHIVE_BYTES', len(data) - 1), fake_http(response):
            with self.assertRaises(helper.FixedBrowserError):
                helper.download_archive(Path(temporary) / 'download.zip')
            self.assertEqual(response.read_sizes, [])
        with tempfile.TemporaryDirectory() as temporary, pinned_fixture(data), fake_http(FakeResponse(data)) as (_, connection):
            connection.request.side_effect = OSError('fake connect failure')
            with self.assertRaises(helper.FixedBrowserError):
                helper.download_archive(Path(temporary) / 'download.zip')
            connection.close.assert_called_once()

    def test_connection_close_response_keeps_valid_pinned_bytes_without_an_active_connection_socket(self):
        data = archive_bytes()
        response = FakeResponse(data, headers=[('Content-Length', str(len(data))), ('Connection', 'close')])
        with REAL_TEMPORARY_DIRECTORY() as temporary, pinned_fixture(data), fake_http(response) as (_, connection):
            connection.sock = None  # HTTPResponse owns the valid closing socket.
            target = Path(temporary) / 'browser.zip'
            helper.download_archive(target)
            self.assertEqual(target.read_bytes(), data)
            self.assertTrue(response.closed)
            connection.close.assert_called_once()

    def test_total_download_alarm_cleans_transport_partial_bytes_and_restores_timer_owner(self):
        data = archive_bytes()
        for phase in ['headers', 'partial-body']:
            with self.subTest(phase=phase), REAL_TEMPORARY_DIRECTORY() as temporary, pinned_fixture(data), \
                    fake_http(FakeResponse(data)) as (_, connection), ExitStack() as stack:
                target = Path(temporary) / 'browser.zip'
                previous = object()
                installed = []

                def install(number, handler):
                    self.assertEqual(number, signal.SIGALRM)
                    if callable(handler):
                        installed.append(handler)
                    return previous
                stack.enter_context(mock.patch.object(helper.signal, 'getitimer', return_value=(0.0, 0.0)))
                stack.enter_context(mock.patch.object(helper.signal, 'getsignal', return_value=previous))
                swap = stack.enter_context(mock.patch.object(helper.signal, 'signal', side_effect=install))
                timer = stack.enter_context(mock.patch.object(helper.signal, 'setitimer'))
                response = connection.getresponse.return_value

                def expire():
                    self.assertEqual(len(installed), 1)
                    installed[0](signal.SIGALRM, None)
                if phase == 'headers':
                    connection.getresponse.side_effect = expire
                else:
                    response.read = mock.Mock(side_effect=[data[:10], lambda: None])

                    def read(_size):
                        if response.stream.tell() == 0:
                            return response.stream.read(10)
                        expire()
                    response.read.side_effect = read
                with self.assertRaises(helper.FixedBrowserError):
                    helper.download_archive(target)
                self.assertFalse(target.exists())
                connection.close.assert_called_once()
                if phase == 'partial-body':
                    self.assertTrue(response.closed)
                self.assertEqual(swap.call_args_list[-1], mock.call(signal.SIGALRM, previous))
                self.assertEqual(timer.call_args_list, [mock.call(signal.ITIMER_REAL, helper.DOWNLOAD_SECONDS),
                    mock.call(signal.ITIMER_REAL, 0), mock.call(signal.ITIMER_REAL, 0.0, 0.0)])

    def test_existing_alarm_refuses_before_network_without_stealing_timer(self):
        with REAL_TEMPORARY_DIRECTORY() as temporary, \
                mock.patch.object(helper.signal, 'getitimer', return_value=(1.0, 0.0)), \
                mock.patch.object(helper.signal, 'signal') as handlers, \
                mock.patch.object(helper.signal, 'setitimer') as timers, \
                mock.patch.object(helper.http.client, 'HTTPSConnection') as network:
            with self.assertRaises(helper.FixedBrowserError):
                helper.download_archive(Path(temporary) / 'browser.zip')
            network.assert_not_called()
            handlers.assert_not_called()
            timers.assert_not_called()

    def test_download_pin_failure_and_transport_cleanup_failure_are_both_retained(self):
        data = archive_bytes()
        response = FakeResponse(data[:-1] + bytes([data[-1] ^ 1]))
        cleanup = OSError('fake response close failure')
        response.close = mock.Mock(side_effect=cleanup)
        with REAL_TEMPORARY_DIRECTORY() as temporary, pinned_fixture(data), fake_http(response) as (_, connection):
            target = Path(temporary) / 'browser.zip'
            with self.assertRaises(BaseExceptionGroup) as raised:
                helper.download_archive(target)
            self.assertEqual(len(raised.exception.exceptions), 2)
            self.assertIsInstance(raised.exception.exceptions[0], helper.FixedBrowserError)
            self.assertIs(raised.exception.exceptions[1], cleanup)
            self.assertFalse(target.exists())
            connection.close.assert_called_once()

    def test_exact_bounded_executable_version_not_just_major_or_channel(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'chrome'
            def script(stdout, stderr=b'', status=0):
                executable.write_text('#!' + sys.executable + '\nimport sys\n' +
                    'assert sys.argv[1:] == ["--version"]\n' +
                    'sys.stdout.buffer.write(' + repr(stdout) + ')\n' +
                    'sys.stderr.buffer.write(' + repr(stderr) + ')\n' +
                    'sys.exit(' + str(status) + ')\n', encoding='utf-8')
                executable.chmod(0o755)
            exact = b'Google Chrome for Testing 157.0.8103.6 \n'
            script(exact)
            before = executable.read_bytes()
            helper.verify_version(executable)
            self.assertEqual(executable.read_bytes(), before)
            for stdout, stderr, status in [(exact, b'', 1),
                    (b'Google Chrome for Testing 157.0.8103.7 \n', b'', 0),
                    (b'Google Chrome 157.0.8103.6\n', b'', 0), (exact + b'extra', b'', 0),
                    (b'x' * 257, b'', 0), (exact, b'x' * 4097, 0)]:
                with self.subTest(stdout_bytes=len(stdout), stderr_bytes=len(stderr), status=status):
                    script(stdout, stderr, status)
                    with self.assertRaises(helper.FixedBrowserError):
                        helper.verify_version(executable)

    def test_version_output_pressure_and_timeout_kill_and_reap_the_owned_fake_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / 'chrome'
            marker = root / 'owned-version.pid'
            for fd in [1, 2, None]:
                with self.subTest(fd=fd):
                    marker.unlink(missing_ok=True)
                    body = '#!' + sys.executable + '\nimport os, signal\nfrom pathlib import Path\n'
                    body += 'Path(' + repr(str(marker)) + ').write_text(str(os.getpid()))\n'
                    if fd is not None:
                        body += 'os.write(' + str(fd) + ', b"x" * 1048576)\n'
                    body += 'signal.pause()\n'
                    executable.write_text(body, encoding='utf-8')
                    executable.chmod(0o755)
                    # This is an actual tiny fixture process, never Chromium.
                    # The short deadline is scoped to this offline control.
                    with mock.patch.object(helper, 'VERSION_TIMEOUT_SECONDS', 0.2), \
                            self.assertRaises(helper.FixedBrowserError):
                        helper.verify_version(executable)
                    self.assertTrue(marker.is_file(), 'owned child reached its controlled blocking point')
                    pid = int(marker.read_text())
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)

    @contextmanager
    def run_resources(self, root, data):
        record = {'owners': [], 'events': [], 'cleanup_error': None, 'temporary_error': None}
        previous = {number: signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        handlers = dict(previous)

        def swap_handler(number, value):
            old = handlers[number]
            handlers[number] = value
            return old

        def own_temporary(**kwargs):
            owner = REAL_TEMPORARY_DIRECTORY(dir=root, **kwargs)
            path = Path(owner.name)
            record['owners'].append(owner)

            def remove():
                record['events'].append('temporary-cleanup')
                owner.cleanup()
                if record['temporary_error']:
                    raise record['temporary_error']
            return SimpleNamespace(name=str(path), cleanup=remove, _finalizer=owner._finalizer)

        def download(target):
            record['events'].append('download')
            self.assertTrue(target.parent.is_dir())
            target.write_bytes(data)

        def verify(executable):
            record['events'].append('version')
            self.assertEqual(executable.read_bytes(), b'fake executable bytes')

        def cleanup():
            record['events'].append('children-cleanup')
            if record['owners']:
                self.assertTrue(Path(record['owners'][0].name).is_dir(),
                                'owned browser bytes cannot disappear before child cleanup')
            if record['cleanup_error']:
                raise record['cleanup_error']

        with ExitStack() as stack:
            stack.enter_context(pinned_fixture(data))
            stack.enter_context(mock.patch.object(helper.platform, 'machine', return_value='x86_64'))
            stack.enter_context(mock.patch.object(helper, 'enable_child_ownership'))
            stack.enter_context(mock.patch.object(helper.process_cleanup, '_direct_children', return_value=[]))
            stack.enter_context(mock.patch.object(helper.signal, 'signal', side_effect=swap_handler))
            stack.enter_context(mock.patch.object(helper.tempfile, 'TemporaryDirectory', side_effect=own_temporary))
            stack.enter_context(mock.patch.object(helper.sys, 'stdout', io.StringIO()))
            record['download'] = stack.enter_context(mock.patch.object(helper, 'download_archive', side_effect=download))
            record['verify'] = stack.enter_context(mock.patch.object(helper, 'verify_version', side_effect=verify))
            record['cleanup'] = stack.enter_context(mock.patch.object(helper, 'cleanup_children', side_effect=cleanup))
            record['launch'] = stack.enter_context(mock.patch.object(helper, 'run_supervisor', return_value=0))
            record['handlers'] = handlers
            record['previous'] = previous
            yield record

    def test_run_changes_only_child_browser_env_and_preserves_exact_native_argv(self):
        data = archive_bytes()
        env = {'PATH': os.environ.get('PATH', ''), 'PLINTH_PG_PASSWORD': 'fake-pg-password',
               'PLINTH_BROWSER': '/fake/old-browser', 'TASK_SENTINEL': 'preserved'}
        for bundle in [None, 'relative bundle with spaces']:
            with self.subTest(bundle=bundle), REAL_TEMPORARY_DIRECTORY() as temporary:
                root = Path(temporary)
                with mock.patch.dict(os.environ, env, clear=True), self.run_resources(root, data) as record:
                    argv = ['--binary', 'relative kernel with spaces']
                    if bundle is not None:
                        argv.extend(['--bundle-dir', bundle])

                    def launch(command, child_env, state):
                        record['events'].append('supervisor-terminal')
                        expected = [sys.executable, str(REPO / 'tests/browser/run-installed-floats.py'), *argv]
                        self.assertEqual(command, expected)
                        self.assertEqual(state['signal'], 0)
                        executable = Path(child_env['PLINTH_BROWSER'])
                        self.assertTrue(executable.is_file())
                        self.assertEqual(child_env, {**env, 'PLINTH_BROWSER': str(executable)})
                        self.assertEqual(dict(os.environ), env, 'never mutate parent/global browser choice')
                        return 0
                    record['launch'].side_effect = launch
                    self.assertEqual(helper.run(argv), 0)
                    record['launch'].assert_called_once()
                    self.assertEqual(record['handlers'], record['previous'])
                    self.assertEqual(record['events'], ['download', 'version', 'supervisor-terminal',
                                                       'children-cleanup', 'temporary-cleanup'])
                    self.assertFalse(Path(record['owners'][0].name).exists())

    def test_child_nonzero_and_preparation_failure_cleanup_never_become_pass(self):
        data = archive_bytes()
        for outcome in ['child', 'download', 'version']:
            with self.subTest(outcome=outcome), REAL_TEMPORARY_DIRECTORY() as temporary:
                with self.run_resources(Path(temporary), data) as record:
                    exact = helper.FixedBrowserError('fake preparation failure')
                    if outcome == 'child':
                        record['launch'].return_value = 37
                        self.assertEqual(helper.run(['--binary', 'fake-kernel']), 37)
                    else:
                        record[outcome if outcome == 'download' else 'verify'].side_effect = exact
                        with self.assertRaises(helper.FixedBrowserError) as raised:
                            helper.run(['--binary', 'fake-kernel'])
                        self.assertIs(raised.exception, exact)
                        record['launch'].assert_not_called()
                    record['cleanup'].assert_called_once()
                    self.assertEqual(record['handlers'], record['previous'])
                    self.assertFalse(Path(record['owners'][0].name).exists())

    def test_child_and_temporary_cleanup_errors_are_failures_preserving_primary(self):
        data = archive_bytes()
        for kind in ['children', 'temporary', 'primary-and-cleanup']:
            with self.subTest(kind=kind), REAL_TEMPORARY_DIRECTORY() as temporary:
                with self.run_resources(Path(temporary), data) as record:
                    primary = helper.FixedBrowserError('fake exact primary')
                    cleanup = OSError('fake exact owned cleanup failure')
                    if kind == 'temporary':
                        record['temporary_error'] = cleanup
                    else:
                        record['cleanup_error'] = cleanup
                    if kind == 'primary-and-cleanup':
                        record['launch'].side_effect = primary
                        with self.assertRaises(BaseExceptionGroup) as raised:
                            helper.run(['--binary', 'fake-kernel'])
                        self.assertEqual(raised.exception.exceptions, (primary, cleanup))
                    else:
                        with self.assertRaises(OSError) as raised:
                            helper.run(['--binary', 'fake-kernel'])
                        self.assertIs(raised.exception, cleanup)
                    self.assertEqual(record['handlers'], record['previous'])
                    self.assertFalse(Path(record['owners'][0].name).exists())

    def test_signal_before_spawn_is_nonzero_and_cleans_private_temporary(self):
        data = archive_bytes()
        with REAL_TEMPORARY_DIRECTORY() as temporary:
            with self.run_resources(Path(temporary), data) as record:
                def interrupted(_target):
                    record['handlers'][signal.SIGTERM](signal.SIGTERM, None)
                record['download'].side_effect = interrupted
                self.assertEqual(helper.run(['--binary', 'fake-kernel']), 128 + signal.SIGTERM)
                record['launch'].assert_not_called()
                self.assertEqual(record['handlers'], record['previous'])
                self.assertFalse(Path(record['owners'][0].name).exists())

    def test_failed_cleanup_retains_browser_bytes_only_when_owned_children_remain_or_are_unknown(self):
        data = archive_bytes()
        for mode in ['remaining', 'uninspectable']:
            with self.subTest(mode=mode), REAL_TEMPORARY_DIRECTORY() as temporary:
                with self.run_resources(Path(temporary), data) as record:
                    with mock.patch.object(helper.process_cleanup, '_direct_children') as inspect:
                        if mode == 'remaining':
                            inspect.return_value = [424242]  # Fake metadata, no real child exists.
                        else:
                            inspect.side_effect = OSError('fake child enumeration failure')
                        with self.assertRaises(BaseExceptionGroup) as raised:
                            helper.run(['--binary', 'fake-kernel'])
                        self.assertGreaterEqual(len(raised.exception.exceptions), 2)
                        self.assertTrue(any(isinstance(error, helper.FixedBrowserError)
                                            for error in raised.exception.exceptions))
                    path = Path(record['owners'][0].name)
                    self.assertTrue(path.is_dir(), 'never delete bytes from under an unjoined native owner')
                    self.assertNotIn('temporary-cleanup', record['events'])
                    self.assertEqual(record['handlers'], record['previous'])
                    # The only children above were mocked. Explicitly recover
                    # this exact test-owned retained directory after inspection.
                    record['owners'][0].cleanup()
                    self.assertFalse(path.exists())

    def test_signal_during_spawn_sends_native_sigint_and_joins_before_temp_cleanup(self):
        data = archive_bytes()
        with REAL_TEMPORARY_DIRECTORY() as temporary:
            with self.run_resources(Path(temporary), data) as record:
                record['launch'].side_effect = REAL_RUN_SUPERVISOR
                child = mock.Mock()
                child.returncode = None
                child.poll.side_effect = lambda: child.returncode

                def wait(timeout=None):
                    self.assertEqual(child.send_signal.call_args_list, [mock.call(signal.SIGINT)])
                    if child.returncode is None:
                        record['events'].append('native-terminal')
                    child.returncode = 130
                    return child.returncode
                child.wait.side_effect = wait

                def spawn(command, **kwargs):
                    record['events'].append('native-spawn')
                    self.assertTrue(kwargs['start_new_session'])
                    self.assertTrue(Path(kwargs['env']['PLINTH_BROWSER']).is_file())
                    self.assertEqual(command[:2], [sys.executable, str(REPO / 'tests/browser/run-installed-floats.py')])
                    record['handlers'][signal.SIGTERM](signal.SIGTERM, None)
                    self.assertNotIn('children-cleanup', record['events'])
                    self.assertTrue(Path(record['owners'][0].name).is_dir())
                    return child
                with mock.patch.object(helper.subprocess, 'Popen', side_effect=spawn) as popen:
                    self.assertEqual(helper.run(['--binary', 'fake-kernel']), 128 + signal.SIGTERM)
                    popen.assert_called_once()
                child.send_signal.assert_called_once_with(signal.SIGINT)
                self.assertEqual(record['events'], ['download', 'version', 'native-spawn', 'native-terminal',
                                                   'children-cleanup', 'temporary-cleanup'])
                state = record['launch'].call_args.args[2]
                self.assertIsNone(state['child'])
                self.assertFalse(state['running'])
                self.assertEqual(record['handlers'], record['previous'])
                self.assertFalse(Path(record['owners'][0].name).exists())

    def test_native_supervisor_outer_expiry_never_passes_after_forced_descendant_cleanup(self):
        child = mock.Mock()
        child.returncode = None
        child.poll.side_effect = lambda: child.returncode
        child.wait.side_effect = lambda timeout=None: child.returncode
        state = {'signal': 0, 'child': None}

        def forced_cleanup():
            child.returncode = -signal.SIGKILL
        with mock.patch.object(helper.subprocess, 'Popen', return_value=child), \
                mock.patch.object(helper, 'SUPERVISOR_SECONDS', 0), \
                mock.patch.object(helper, 'STOP_SECONDS', 0), \
                mock.patch.object(helper, 'cleanup_children', side_effect=forced_cleanup) as cleanup:
            with self.assertRaises(BaseExceptionGroup) as raised:
                REAL_RUN_SUPERVISOR(['fake-supervisor'], {'TASK_SENTINEL': 'fake'}, state)
            self.assertTrue(all(isinstance(error, helper.FixedBrowserError) for error in raised.exception.exceptions))
            self.assertEqual(len(raised.exception.exceptions), 2)
            child.send_signal.assert_called_once_with(signal.SIGINT)
            cleanup.assert_called_once()
            self.assertIsNone(state['child'])
            self.assertFalse(state['running'])

    def test_unsupported_platform_refuses_before_network_or_subprocess(self):
        for system, machine in [('linux', 'aarch64'), ('darwin', 'x86_64')]:
            with self.subTest(system=system, machine=machine), \
                    mock.patch.object(helper.sys, 'platform', system), \
                    mock.patch.object(helper.platform, 'machine', return_value=machine), \
                    mock.patch.object(helper, 'download_archive') as download, \
                    mock.patch.object(helper, 'run_supervisor') as child:
                with self.assertRaises(helper.FixedBrowserError):
                    helper.run(['--binary', 'fake-kernel'])
                download.assert_not_called()
                child.assert_not_called()


if __name__ == '__main__':
    unittest.main()
