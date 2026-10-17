#!/usr/bin/env python3
"""Own one exact test-only browser for the existing native float supervisor."""

import argparse
import ctypes
import hashlib
import http.client
import os
from pathlib import Path
import platform
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time
import zipfile


BROWSER_VERSION = '157.0.8103.6'
BROWSER_URL = 'https://storage.googleapis.com/chrome-for-testing-public/157.0.8103.6/linux64/chrome-linux64.zip'
ARCHIVE_SIZE = 202676171
ARCHIVE_SHA256 = '7802b25411aa5b8aa387cc733a45df6eed1aea8cc046fd4e2dec64d5918b1853'
EXPECTED_VERSION = b'Google Chrome for Testing 157.0.8103.6 \n'
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_ENTRIES = 512
MAX_EXPANDED_BYTES = 512 * 1024 * 1024
MAX_MEMBER_BYTES = 320 * 1024 * 1024
MAX_PATH_BYTES = 256
MAX_DEPTH = 8
DOWNLOAD_SECONDS = 180.0
IO_SECONDS = 15.0
VERSION_TIMEOUT_SECONDS = 10.0
SUPERVISOR_SECONDS = 1200.0
STOP_SECONDS = 360.0
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/browser'))
import process_cleanup  # noqa: E402 -- reuse the existing owned descendant protocol


class FixedBrowserError(RuntimeError):
    """A fixed, non-sensitive failure of this test-tool owner."""


class BrowserInterrupted(BaseException):
    pass


def _raise_failures(failures):
    if len(failures) == 1:
        raise failures[0]
    if failures:
        raise BaseExceptionGroup('fixed browser operation and cleanup failed', failures)


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise FixedBrowserError('fixed browser operation exceeded its deadline')
    return remaining


def download_archive(destination: Path) -> None:
    """No proxy, redirect, channel resolution or alternate artifact is used."""
    previous_timer = signal.getitimer(signal.ITIMER_REAL)
    if previous_timer != (0.0, 0.0):
        raise FixedBrowserError('fixed browser download requires its own deadline timer')
    previous_alarm = signal.getsignal(signal.SIGALRM)
    connection = None
    response = None
    created = False
    failures = []

    def expired(_signum, _frame):
        raise FixedBrowserError('fixed browser download exceeded its total deadline')

    try:
        signal.signal(signal.SIGALRM, expired)
        signal.setitimer(signal.ITIMER_REAL, DOWNLOAD_SECONDS)
        deadline = time.monotonic() + DOWNLOAD_SECONDS
        connection = http.client.HTTPSConnection('storage.googleapis.com', timeout=IO_SECONDS)
        connection.request('GET', BROWSER_URL.removeprefix('https://storage.googleapis.com'),
                           headers={'Accept-Encoding': 'identity'})
        response = connection.getresponse()
        headers = response.getheaders()
        if response.status != 200 or sum(len(k) + len(v) for k, v in headers) > 16384:
            raise FixedBrowserError('fixed browser download response refused')
        lengths = [v for k, v in headers if k.lower() == 'content-length']
        encodings = [v for k, v in headers if k.lower() == 'content-encoding']
        if lengths != [str(ARCHIVE_SIZE)] or any(v.lower() != 'identity' for v in encodings):
            raise FixedBrowserError('fixed browser download length or encoding refused')
        if ARCHIVE_SIZE > MAX_ARCHIVE_BYTES:
            raise FixedBrowserError('fixed browser compressed size exceeds limit')
        descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        created = True
        digest = hashlib.sha256()
        count = 0
        with os.fdopen(descriptor, 'wb') as output:
            while True:
                remaining = _remaining(deadline)
                if connection.sock is not None:
                    connection.sock.settimeout(min(IO_SECONDS, remaining))
                chunk = response.read(65536)
                if not chunk:
                    break
                count += len(chunk)
                if count > ARCHIVE_SIZE or count > MAX_ARCHIVE_BYTES:
                    raise FixedBrowserError('fixed browser download exceeds size')
                digest.update(chunk)
                output.write(chunk)
        _remaining(deadline)
        if count != ARCHIVE_SIZE or digest.hexdigest() != ARCHIVE_SHA256:
            raise FixedBrowserError('fixed browser archive pin mismatch')
    except BaseException as error:
        if isinstance(error, Exception) and not isinstance(error, FixedBrowserError):
            wrapped = FixedBrowserError('fixed browser download failed')
            wrapped.__cause__ = error
            error = wrapped
        failures.append(error)
        if created:
            try:
                destination.unlink(missing_ok=True)
            except BaseException as cleanup_error:
                failures.append(cleanup_error)
    finally:
        try:
            signal.setitimer(signal.ITIMER_REAL, 0)
        except BaseException as error:
            failures.append(error)
        if response is not None:
            try:
                response.close()
            except BaseException as error:
                failures.append(error)
        if connection is not None:
            try:
                connection.close()
            except BaseException as error:
                failures.append(error)
        try:
            signal.signal(signal.SIGALRM, previous_alarm)
            signal.setitimer(signal.ITIMER_REAL, *previous_timer)
        except BaseException as error:
            failures.append(error)
    _raise_failures(failures)


def _verified_archive(source):
    metadata = os.fstat(source.fileno())
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != ARCHIVE_SIZE or metadata.st_size > MAX_ARCHIVE_BYTES:
        raise FixedBrowserError('fixed browser archive size refused')
    digest = hashlib.sha256()
    count = 0
    while chunk := source.read(65536):
        count += len(chunk)
        if count > ARCHIVE_SIZE:
            raise FixedBrowserError('fixed browser archive grew')
        digest.update(chunk)
    if count != ARCHIVE_SIZE or digest.hexdigest() != ARCHIVE_SHA256:
        raise FixedBrowserError('fixed browser archive pin mismatch')
    source.seek(0)


def _inventory(archive):
    entries = archive.infolist()
    if not entries or len(entries) > MAX_ENTRIES:
        raise FixedBrowserError('fixed browser ZIP entry limit')
    nodes = {}
    total = 0
    for info in entries:
        name = info.filename
        parts = name.removesuffix('/').split('/')
        mode = info.external_attr >> 16
        directory = info.is_dir()
        if (info.orig_filename != name or '\x00' in name or '\\' in name or ':' in name
                or len(name.encode('utf-8')) > MAX_PATH_BYTES or len(parts) > MAX_DEPTH
                or parts[0] != 'chrome-linux64' or any(p in ('', '.', '..') for p in parts)
                or info.flag_bits & 1 or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                or info.create_system != 3 or mode & 0o7000
                or (directory and (not stat.S_ISDIR(mode) or stat.S_IMODE(mode) != 0o755))
                or (not directory and (not stat.S_ISREG(mode) or stat.S_IMODE(mode) not in (0o600, 0o644, 0o755)))
                or info.file_size > MAX_MEMBER_BYTES or info.file_size < 0
                or (directory and (info.file_size != 0 or info.compress_size != 0))):
            raise FixedBrowserError('fixed browser ZIP metadata refused')
        key = '/'.join(parts)
        if key in nodes:
            raise FixedBrowserError('fixed browser ZIP duplicate refused')
        nodes[key] = directory
        total += info.file_size
        if total > MAX_EXPANDED_BYTES:
            raise FixedBrowserError('fixed browser ZIP expanded limit')
    for key in nodes:
        parents = key.split('/')[:-1]
        for length in range(1, len(parents) + 1):
            parent = '/'.join(parents[:length])
            if parent in nodes and not nodes[parent]:
                raise FixedBrowserError('fixed browser ZIP hierarchy conflict')
    executable = next((i for i in entries if i.filename == 'chrome-linux64/chrome'), None)
    if executable is None or executable.is_dir() or stat.S_IMODE(executable.external_attr >> 16) != 0o755:
        raise FixedBrowserError('fixed browser executable missing or invalid')
    return entries


def extract_archive(archive: Path, destination: Path) -> Path:
    """Verify the whole pin first, then validate the whole bounded inventory."""
    try:
        with archive.open('rb') as source:
            _verified_archive(source)
            with zipfile.ZipFile(source) as zipped:
                entries = _inventory(zipped)
                destination.mkdir(mode=0o700)  # Never reuse another owner's output.
                actual_total = 0
                for info in entries:
                    output = destination.joinpath(*info.filename.removesuffix('/').split('/'))
                    if info.is_dir():
                        output.mkdir(mode=0o755, parents=True, exist_ok=True)
                        continue
                    output.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
                    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                    actual = 0
                    with os.fdopen(descriptor, 'wb') as target, zipped.open(info) as member:
                        while chunk := member.read(65536):
                            actual += len(chunk)
                            actual_total += len(chunk)
                            if actual > info.file_size or actual > MAX_MEMBER_BYTES or actual_total > MAX_EXPANDED_BYTES:
                                raise FixedBrowserError('fixed browser ZIP stream exceeds limit')
                            target.write(chunk)
                        if actual != info.file_size:
                            raise FixedBrowserError('fixed browser ZIP stream truncated')
                        os.fchmod(target.fileno(), stat.S_IMODE(info.external_attr >> 16))
        return destination / 'chrome-linux64/chrome'
    except FixedBrowserError:
        raise
    except Exception as error:
        raise FixedBrowserError('fixed browser extraction failed') from error


def verify_version(executable: Path) -> None:
    """Keep output physically bounded, including on a misbehaving executable."""
    try:
        metadata = executable.lstat()
        if not stat.S_ISREG(metadata.st_mode) or not metadata.st_mode & stat.S_IXUSR:
            raise FixedBrowserError('fixed browser executable refused')
        child = subprocess.Popen([str(executable), '--version'], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
    except OSError as error:
        raise FixedBrowserError('fixed browser version process unavailable') from error
    failures = []
    try:
        deadline = time.monotonic() + VERSION_TIMEOUT_SECONDS
        output = bytearray()
        stderr_bytes = 0
        with selectors.DefaultSelector() as selector:
            for stream in (child.stdout, child.stderr):
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ)
            while selector.get_map():
                ready = selector.select(_remaining(deadline))
                if not ready:
                    raise FixedBrowserError('fixed browser version deadline')
                for key, _ in ready:
                    chunk = os.read(key.fileobj.fileno(), 4096)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    elif key.fileobj is child.stdout:
                        if len(output) + len(chunk) > 256:
                            raise FixedBrowserError('fixed browser version output limit')
                        output.extend(chunk)
                    else:
                        stderr_bytes += len(chunk)
                        if stderr_bytes > 4096:
                            raise FixedBrowserError('fixed browser version error output limit')
        result = child.wait(timeout=_remaining(deadline))
        if result != 0 or bytes(output) != EXPECTED_VERSION or stderr_bytes:
            raise FixedBrowserError('fixed browser version mismatch')
    except BaseException as error:
        if isinstance(error, Exception) and not isinstance(error, FixedBrowserError):
            wrapped = FixedBrowserError('fixed browser version check failed')
            wrapped.__cause__ = error
            error = wrapped
        failures.append(error)
    finally:
        try:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
        except BaseException as error:
            failures.append(error)
        for stream in (child.stdout, child.stderr):
            try:
                stream.close()
            except BaseException as error:
                failures.append(error)
    _raise_failures(failures)


def enable_child_ownership():
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise FixedBrowserError('fixed browser child ownership unavailable')
    if process_cleanup._direct_children():
        raise FixedBrowserError('fixed browser owner must start without children')


def cleanup_children():
    process_cleanup._clean_descendants()


def run_supervisor(command, env, stop_state) -> int:
    """Give the native owner SIGINT so its database/protected finally runs."""
    stop_state['running'] = True
    child = subprocess.Popen(command, env=env, start_new_session=True)
    stop_state['child'] = child
    deadline = time.monotonic() + SUPERVISOR_SECONDS
    stopping = False
    forced = False
    failures = []
    result = 1
    try:
        while child.poll() is None:
            if not stopping and (stop_state['signal'] or time.monotonic() >= deadline):
                forced = not stop_state['signal']
                child.send_signal(signal.SIGINT)
                stopping = True
                deadline = time.monotonic() + STOP_SECONDS
            if stopping and time.monotonic() >= deadline:
                raise FixedBrowserError('native supervisor cleanup exceeded its bound')
            try:
                child.wait(timeout=min(0.2, max(0.001, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                pass
        result = child.wait()
        if forced:
            raise FixedBrowserError('native supervisor exceeded its outer bound')
        result = 128 + stop_state['signal'] if stop_state['signal'] else (result if result >= 0 else 128 - result)
    except BaseException as error:
        failures.append(error)
    finally:
        try:
            if child.poll() is None and not stopping:
                child.send_signal(signal.SIGINT)
                try:
                    child.wait(timeout=STOP_SECONDS)
                except subprocess.TimeoutExpired:
                    pass
            # Forced descendants are a failure, never native DB cleanup proof.
            if child.poll() is None:
                cleanup_children()
                child.wait(timeout=5)
                failures.append(FixedBrowserError('native supervisor required forced cleanup'))
        except BaseException as error:
            failures.append(error)
        stop_state['child'] = None
        stop_state['running'] = False
    _raise_failures(failures)
    return result


def run(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--bundle-dir')
    args = parser.parse_args(argv)
    if sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        raise FixedBrowserError('fixed browser requires Linux x86_64')
    state = {'signal': 0, 'child': None}
    handlers = {}
    temporary = None
    failures = []
    result = 1
    supervisor_result = None

    def stopped(signum, _frame):
        if not state['signal']:
            state['signal'] = signum
        if state['child'] is None and not state.get('running', False):
            raise BrowserInterrupted()

    enable_child_ownership()
    try:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            handlers[signum] = signal.signal(signum, stopped)
        temporary = tempfile.TemporaryDirectory(prefix='plinth-fixed-browser-')
        root = Path(temporary.name)
        root.chmod(0o700)
        archive = root / 'browser.zip'
        download_archive(archive)
        executable = extract_archive(archive, root / 'unpacked')
        verify_version(executable)
        print(f'fixed browser verified version={BROWSER_VERSION} sha256={ARCHIVE_SHA256}', flush=True)
        command = [sys.executable, str(REPO / 'tests/browser/run-installed-floats.py'), '--binary', args.binary]
        if args.bundle_dir is not None:
            command.extend(['--bundle-dir', args.bundle_dir])
        env = os.environ.copy()
        env['PLINTH_BROWSER'] = str(executable)
        result = run_supervisor(command, env, state)
        supervisor_result = result
    except BrowserInterrupted:
        result = 128 + state['signal']
    except BaseException as error:
        failures.append(error)
    finally:
        # Ignore repeated interruptions only while completing owned cleanup.
        for signum in handlers:
            try:
                signal.signal(signum, signal.SIG_IGN)
            except BaseException as error:
                failures.append(error)
        try:
            had_children = bool(process_cleanup._direct_children())
            cleanup_children()
            if had_children:
                failures.append(FixedBrowserError('fixed browser had unjoined owned descendants'))
        except BaseException as error:
            failures.append(error)
        can_remove = False
        try:
            can_remove = not process_cleanup._direct_children()
        except BaseException as error:
            failures.append(error)
        if temporary is not None:
            if can_remove:
                try:
                    temporary.cleanup()
                except BaseException as error:
                    failures.append(error)
            else:
                # A failed owner must not unlink a browser still used by its
                # children. Retention is an explicit failure, never a pass.
                temporary._finalizer.detach()
                failures.append(FixedBrowserError('owned children remain or cannot be inspected; owned temporary retained'))
        for signum, previous in handlers.items():
            try:
                signal.signal(signum, previous)
            except BaseException as error:
                failures.append(error)
    if failures and supervisor_result not in (None, 0):
        failures.insert(0, FixedBrowserError(f'native supervisor exited with status {supervisor_result}'))
    _raise_failures(failures)
    return 128 + state['signal'] if state['signal'] else result


if __name__ == '__main__':
    try:
        status = run()
    except SystemExit:
        # Preserve argparse's successful --help and invalid-argument statuses.
        raise
    except BaseException:
        # Tool/network/process exceptions may contain inherited environment or
        # remote output. Keep detailed objects out of shared CI log rendering.
        print('fixed browser preparation or owned journey failed', file=sys.stderr)
        status = 1
    raise SystemExit(status)
