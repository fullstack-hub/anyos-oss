"""Cooperative leases for canonical mutable artifact slots.

The lock inode lives outside the replaced output. Official writers must hold
exclusive leases throughout build/sign/publication; a selected engine holds
shared leases throughout its life. This is not a sandbox against same-UID code.
"""
from __future__ import annotations
import contextlib
import contextvars
import ctypes
import functools
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import stat
import time
import sys
import subprocess

DOMAIN = b"anyos-artifact-slot-v1\0"
TIMEOUT = 30.0


@functools.lru_cache(maxsize=1)
def _darwin_cf():
    # System framework, no package dependency. Use an explicit invariant locale
    # in both languages, not the caller's language or Python Unicode version.
    cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    ptr, index, encoding = ctypes.c_void_p, ctypes.c_long, ctypes.c_uint32
    signatures = {
        'CFStringCreateWithBytes': (ptr, [ptr, ptr, index, encoding, ctypes.c_ubyte]),
        'CFStringCreateMutableCopy': (ptr, [ptr, index, ptr]),
        'CFLocaleCreate': (ptr, [ptr, ptr]),
        'CFStringNormalize': (None, [ptr, ctypes.c_int]),
        'CFStringFold': (None, [ptr, ctypes.c_ulong, ptr]),
        'CFStringGetLength': (index, [ptr]),
        'CFStringGetMaximumSizeForEncoding': (index, [index, encoding]),
        'CFStringGetCString': (ctypes.c_ubyte, [ptr, ptr, index, encoding]),
        'CFRelease': (None, [ptr]),
    }
    for name, (result, args) in signatures.items():
        function = getattr(cf, name); function.restype = result; function.argtypes = args
    return cf


def _darwin_fold(part):
    cf, refs = _darwin_cf(), []
    utf8 = 0x08000100
    try:
        original = cf.CFStringCreateWithBytes(None, part, len(part), utf8, 0)
        if not original: raise ValueError('slot is not valid UTF-8')
        refs.append(original)
        mutable = cf.CFStringCreateMutableCopy(None, 0, original)
        if not mutable: raise MemoryError('slot normalization unavailable')
        refs.append(mutable)
        identifier = b'en_US_POSIX'
        locale_id = cf.CFStringCreateWithBytes(None, identifier, len(identifier), utf8, 0)
        if not locale_id: raise MemoryError('invariant locale unavailable')
        refs.append(locale_id)
        locale = cf.CFLocaleCreate(None, locale_id)
        if not locale: raise MemoryError('invariant locale unavailable')
        refs.append(locale)
        cf.CFStringNormalize(mutable, 0)  # canonical decomposition, not compatibility
        cf.CFStringFold(mutable, 1, locale)  # case only; preserve accents and width
        cf.CFStringNormalize(mutable, 0)
        capacity = cf.CFStringGetMaximumSizeForEncoding(cf.CFStringGetLength(mutable), utf8) + 1
        if capacity <= 0: raise ValueError('slot normalization failed')
        result = ctypes.create_string_buffer(capacity)
        if not cf.CFStringGetCString(mutable, result, capacity, utf8):
            raise ValueError('slot normalization failed')
        return result.value
    finally:
        for ref in reversed(refs): cf.CFRelease(ref)


def _canonical_components(slot: Path):
    path = os.fspath(slot)
    if not os.path.isabs(path) or any(p in (".", "..") for p in path.split("/")):
        raise ValueError("artifact slot must be an absolute normalized path")
    parts = [p for p in os.fsencode(path).split(b"/") if p]
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    normalized, sensitivities = [], []
    missing = False
    try:
        for part in parts:
            # Darwin _PC_CASE_SENSITIVE (sys/unistd.h). A missing suffix uses
            # its last existing parent filesystem. Fold before creation too,
            # so directory publication/replacement never changes the key.
            sensitive = os.fpathconf(fd, 11) if sys.platform == "darwin" else 1
            if sensitive not in (0, 1):
                raise ValueError("filesystem case sensitivity unavailable")
            sensitivities.append(bool(sensitive))
            normalized.append(part if sensitive else _darwin_fold(part))
            if missing:
                continue
            try:
                nxt = os.open(part, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            except FileNotFoundError:
                missing = True
                continue
            os.close(fd)
            fd = nxt
    finally:
        os.close(fd)
    return normalized, sensitivities


def canonical_slot(slot: Path) -> bytes:
    normalized, _ = _canonical_components(slot)
    return b"/" + b"/".join(normalized)


def runtime_slot(path: Path) -> Path:
    """Resolve every official mutable artifact root from its on-disk layout.

    Root and descendant spelling share a key, independently of the executing
    checkout. The installer has four prefix inputs and arbitrary runtime
    bundle targets (Contents/Frameworks); custom libraries outside these roots
    are locked by their containing output directory without widening to its
    unrelated parent. Symlink ancestors are rejected by canonical_slot.
    """
    parts, sensitivities = _canonical_components(path)
    canonical = [b'', *parts]
    original = Path(path).parts
    prefixes = ((b'.runtime', b'gpu-build', b'prefix'),
                (b'.runtime', b'gfxstream-stage1', b'prefix-final'),
                (b'.runtime', b'gfxstream-stage1', b'protobuf-prefix'),
                (b'.runtime', b'libslirp', b'prefix'))
    for i in range(1, len(canonical)):
        if tuple(canonical[i:i+3]) in prefixes:
            return Path(*original[:i+3])
        if tuple(canonical[i:i+2]) == (b'.runtime', b'anyos-vmm'):
            return Path(*original[:i+2])
        if i > 1 and len(canonical[i:i+2]) == 2:
            # Interpret only components of the validated path. Never probe a
            # hypothetical sibling Contents/Frameworks of a custom output.
            contents = b'Contents' if sensitivities[i-1] else b'contents'
            frameworks = b'Frameworks' if sensitivities[i] else b'frameworks'
            if canonical[i:i+2] == [contents, frameworks]:
                return Path(*original[:i])
    return Path(path)


def slot_key(slot: Path) -> str:
    return hashlib.sha256(DOMAIN + canonical_slot(runtime_slot(slot))).hexdigest()


_ACTIVE = contextvars.ContextVar("artifact_lease_descriptors", default=())


def child_options(*, env=None, pass_fds=()):
    """Only descriptors actually owned by a live validated lease context.

    Every mutation subprocess keeps the open-file descriptions independently
    of the Python writer's lifetime. Nested official writers validate these
    descriptors against the canonical lock inode before re-entering.
    """
    entries = _ACTIVE.get()
    if not entries:
        return {"env": env, "pass_fds": tuple(pass_fds)}
    inherited = dict(os.environ if env is None else env)
    inherited["ANYOS_ARTIFACT_WRITER_LEASES"] = json.dumps([
        {"fd": fd, "key": key} for fd, key, mode in entries if mode == fcntl.LOCK_EX
    ])
    return {"env": inherited, "pass_fds": tuple(sorted(set(pass_fds) | {fd for fd, _, _ in entries}))}


def run(command, **kwargs):
    if kwargs.get("shell"):
        raise ValueError("artifact subprocess requires structured argv")
    kwargs.update(child_options(env=kwargs.pop("env", None), pass_fds=kwargs.pop("pass_fds", ())))
    return subprocess.run(command, **kwargs)


def store_path() -> Path:
    # Actual account home, not an inherited HOME or artifact-relative path.
    return Path(pwd.getpwuid(os.geteuid()).pw_dir) / ".cache/anyos/artifact-leases"


def _store(path: Path) -> int:
    if not path.is_absolute() or any(p in (".", "..") for p in path.parts):
        raise ValueError("lease store must be absolute")
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.parts[1:]:
            created = False
            try:
                os.mkdir(part, 0o700, dir_fd=fd)
                created = True
            except FileExistsError:
                pass
            nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            st = os.fstat(nxt)
            # Root's sticky /tmp is an allowed ancestor, never the lock store.
            sticky_root = st.st_uid == 0 and bool(st.st_mode & stat.S_ISVTX)
            if st.st_uid not in (0, os.geteuid()) or (st.st_mode & 0o022 and not sticky_root):
                os.close(nxt)
                raise PermissionError("unsafe lease store ancestor")
            if created:
                try:
                    os.fchmod(nxt, 0o700)
                except BaseException:
                    os.close(nxt)
                    raise
            os.close(fd)
            fd = nxt
        st = os.fstat(fd)
        if st.st_uid != os.geteuid() or stat.S_IMODE(st.st_mode) != 0o700:
            raise PermissionError("lease store must be private and owned")
        return fd
    except BaseException:
        os.close(fd)
        raise


@contextlib.contextmanager
def leases(read=(), write=(), *, timeout=TIMEOUT, store=None):
    """Acquire all distinct keys in one order. Never unlink a lock file."""
    if not 0 <= timeout <= 300:
        raise ValueError("invalid bounded lease timeout")
    modes = {slot_key(runtime_slot(Path(p))): fcntl.LOCK_SH for p in read}
    modes.update({slot_key(runtime_slot(Path(p))): fcntl.LOCK_EX for p in write})
    directory = _store(Path(store) if store is not None else store_path())
    held = []
    deadline = time.monotonic() + timeout
    try:
        inherited = json.loads(os.environ.get("ANYOS_ARTIFACT_WRITER_LEASES", "[]"))
        if not isinstance(inherited, list) or len(inherited) > 64:
            raise ValueError("invalid inherited writer leases")
        inherited_by_key = {}
        for item in inherited:
            if not isinstance(item, dict) or set(item) != {"fd", "key"} or not isinstance(item["fd"], int) or item["fd"] <= 2 or item["key"] in inherited_by_key:
                raise ValueError("invalid inherited writer lease")
            inherited_by_key[item["key"]] = item["fd"]
        for key, mode in sorted(modes.items()):
            name = key + ".lock"
            parent_fd = inherited_by_key.get(key)
            if parent_fd is not None:
                # Dup shares the original open-file description (and flock).
                # Validate it against this canonical private store, not a bool.
                fd = os.dup(parent_fd)
                held.append(fd)
                os.set_inheritable(fd, False)
            else:
                fd = -1
            try:
                if fd < 0:
                    fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=directory)
                    held.append(fd)
                    os.fchmod(fd, 0o600)
            except FileExistsError:
                fd = os.open(name, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory)
                held.append(fd)
            st = os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or st.st_nlink != 1 or stat.S_IMODE(st.st_mode) != 0o600:
                raise PermissionError("unsafe artifact lease file")
            live = os.stat(name, dir_fd=directory, follow_symlinks=False)
            if (live.st_dev, live.st_ino) != (st.st_dev, st.st_ino):
                raise PermissionError("artifact lock inode changed")
            while True:
                try:
                    fcntl.flock(fd, (fcntl.LOCK_EX if parent_fd is not None else mode) | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("artifact slot is leased by a running reader or writer")
                    time.sleep(min(0.02, max(0, deadline - time.monotonic())))
            live = os.stat(name, dir_fd=directory, follow_symlinks=False)
            if (live.st_dev, live.st_ino) != (st.st_dev, st.st_ino):
                raise PermissionError("artifact lock inode changed")
        live_dir = _store(Path(store) if store is not None else store_path())
        try:
            live, original = os.fstat(live_dir), os.fstat(directory)
            if (live.st_dev, live.st_ino) != (original.st_dev, original.st_ino):
                raise PermissionError("lease store changed")
        finally:
            os.close(live_dir)
        token = _ACTIVE.set(_ACTIVE.get() + tuple((fd, key, modes[key]) for key, fd in zip(sorted(modes), held)))
        try:
            yield tuple(held)
        finally:
            _ACTIVE.reset(token)
    finally:
        for fd in reversed(held):
            os.close(fd)
        os.close(directory)


if __name__ == "__main__":
    import argparse
    import subprocess
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", type=Path, action="append", default=[])
    parser.add_argument("--read", type=Path, action="append", default=[])
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not (args.write or args.read):
        parser.error("leases and command argv required")
    with leases(read=args.read, write=args.write):
        raise SystemExit(run(command).returncode)
