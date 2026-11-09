#!/usr/bin/env python3.13
"""Meson build directories of the engine's GPU runtime libraries.

    python3.13 scripts/gpu_build_dirs.py [--stage DIR] [--product DIR] component ...
        components: gfxstream, rutabaga, virglrenderer, dxmt

A meson build directory records absolute paths: its source and build
directories, the prefix, the `pkg_config_path` and every dependency's include
and library paths found at configure time.  After the checkout moved (the
`.runtime/gpu-build` tree was first configured in an earlier checkout
directory), ninja either fails to regenerate
(`build/dxmt`) or links against libraries that no longer exist
(`build-rutabaga-dual`), and the gfxstream directory existed only where
scripts/dev/integration2/build.sh had created it.  This module is the one
place those directories' meson options live; scripts/build-gpu-runtime-libs.py
calls ensure() before each meson build and integration2/build.sh calls this
command line.

The prefixes' pkg-config files carry their prefix as an absolute path too
(epoxy.pc pointed at the old checkout after the move).  Before configuring,
ensure() rewrites a `.pc` whose prefix is the same `.runtime/...` folder in
another checkout to this checkout's (the old file kept as `.pc.old-<id>`);
a prefix outside any `.runtime` (vulkan.pc's build prefix) is left as it is.

ensure() leaves a directory configured for the current paths alone (so
incremental builds stay incremental).  A directory that is missing, that was
configured for another source or build path, whose prefix or pkg_config_path
is not the current one, or whose recorded dependency paths no longer exist is
set aside as `<dir>.old-<id>` and configured fresh with the options below.

Trees are the same as scripts/stage-gpu-sources.py's: --stage (default
.runtime/gfxstream-stage1) holds gfxstream and rutabaga_gfx, --product
(default .runtime/gpu-build) holds src/virglrenderer and src/dxmt.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Callable, NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import artifact_lease  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HOMEBREW = Path("/opt/homebrew")


class BuildDir(NamedTuple):
    build: Path
    source: Path
    options: dict[str, str]
    # The meson invocation and directories put first on PATH for configure and
    # the generated build steps.  virglrenderer's code generators need PyYAML,
    # which only the product's venv has; its venv/bin/meson script carries an
    # absolute shebang that breaks when the tree moves, so meson runs as a
    # module of the venv interpreter instead.
    meson: tuple[str, ...]
    path: tuple[Path, ...] = ()


def build_dirs(stage: Path, product: Path) -> dict[str, BuildDir]:
    stage, product = Path(stage), Path(product)
    meson = (str(HOMEBREW / "bin/meson"),)
    venv = product / "venv/bin"
    return {
        "gfxstream": BuildDir(
            stage / "build-host-final", stage / "gfxstream",
            {"buildtype": "release", "default_library": "shared",
             "prefix": str(stage / "prefix-final"), "snapshot-support": "true", "cpp_std": "c++17",
             "pkg_config_path": f"{stage / 'protobuf-prefix/lib/pkgconfig'}:{HOMEBREW / 'lib/pkgconfig'}"},
            meson),
        "rutabaga": BuildDir(
            stage / "build-rutabaga-dual", stage / "rutabaga_gfx",
            # b_ndebug=true: meson's default (false) passes `-C debug-assertions=yes`
            # to rustc even for buildtype=release, which compiles in upstream's
            # `if cfg!(debug_assertions) { dup2(STDOUT_FILENO, STDERR_FILENO) }`
            # in VirglRenderer::init and overflow checks (#855).
            {"buildtype": "release", "b_ndebug": "true", "prefix": str(stage / "prefix-final"),
             "features": "gfxstream,virgl_renderer", "ffi": "true", "build-tests": "false",
             "pkg_config_path": ":".join(str(p) for p in (
                 stage / "prefix-final/lib/pkgconfig", product / "prefix/lib/pkgconfig",
                 stage / "protobuf-prefix/lib/pkgconfig"))},
            meson),
        "virglrenderer": BuildDir(
            product / "build/virgl", product / "src/virglrenderer",
            {"buildtype": "release",
             "c_args": f"-I{product / 'src/webkit/Source/ThirdParty/ANGLE/include'}",
             "venus": "true", "neptune": "true", "render-server-mode": "process",
             "tests": "false", "check-gl-errors": "false",
             "prefix": str(product / "prefix"), "libdir": "lib", "libexecdir": "libexec",
             "pkg_config_path": str(product / "prefix/lib/pkgconfig")},
            (str(venv / "python3.13"), "-m", "mesonbuild.mesonmain"), (venv,)),
        "dxmt": BuildDir(
            product / "build/dxmt", product / "src/dxmt",
            {"buildtype": "release", "native_llvm_path": str(HOMEBREW / "opt/llvm@15"),
             "prefix": str(product / "prefix")},
            meson),
    }


def _same(a, b: Path) -> bool:
    try:
        return Path(a).resolve() == b.resolve()
    except (OSError, TypeError):
        return False


def _option_text(value) -> str:
    # intro-buildoptions keeps an array option as a list (pkg_config_path as
    # one colon-joined element).
    return ":".join(map(str, value)) if isinstance(value, list) else str(value)


def _dependency_paths(dependencies: list) -> list[str]:
    paths = []
    for dependency in dependencies:
        for arg in [*dependency.get("compile_args", []), *dependency.get("link_args", [])]:
            for flag in ("-I", "-L"):
                if arg.startswith(flag + "/"):
                    arg = arg[len(flag):]
            if arg.startswith("/"):
                paths.append(arg)
    return paths


def relocate_pkgconfig(directory: Path) -> list[Path]:
    """Point the `.pc` files of the prefix `directory` (<prefix>/lib/pkgconfig)
    belongs to at that prefix when they name the same `.runtime` folder of
    another checkout.  Returns the files rewritten."""
    directory = Path(directory)
    prefix = directory.parent.parent
    if directory.name != "pkgconfig" or directory.parent.name != "lib" or ".runtime" not in prefix.parts:
        return []
    relative = prefix.parts[len(prefix.parts) - prefix.parts[::-1].index(".runtime") - 1:]
    rewritten = []
    for pc in sorted(directory.glob("*.pc")):
        text = pc.read_text()
        old = next((line[len("prefix="):].strip() for line in text.splitlines() if line.startswith("prefix=")), None)
        if not old or old == str(prefix) or Path(old).parts[-len(relative):] != relative:
            continue
        staged = pc.with_name(pc.name + ".pending")
        staged.write_text(text.replace(old, str(prefix)))
        pc.rename(pc.with_name(pc.name + ".old-" + uuid.uuid4().hex))
        staged.rename(pc)
        rewritten.append(pc)
    return rewritten


def pkgconfig_dirs(spec: BuildDir) -> list[Path]:
    """The prefixes' pkg-config directories `spec` configures against or installs to."""
    dirs = [Path(p) for p in spec.options.get("pkg_config_path", "").split(":") if p]
    if "prefix" in spec.options:
        dirs.append(Path(spec.options["prefix"]) / "lib/pkgconfig")
    return list(dict.fromkeys(dirs))


def lease_slots(name: str, stage: Path, product: Path) -> list[Path]:
    """The prefixes under `stage`/`product` ensure(name) may change
    (pkg-config files it relocates, the prefix it configures for).  Callers hold writer leases on
    them around ensure() (scripts/artifact_lease.py): build-gpu-runtime-libs.py,
    integration2/build.sh and this command line."""
    spec = build_dirs(stage, product)[name]
    return [d.parent.parent for d in pkgconfig_dirs(spec)
            if d.is_relative_to(Path(stage)) or d.is_relative_to(Path(product))]


def problem(spec: BuildDir) -> str | None:
    """Why `spec.build` must be configured again, or None when it is current."""
    info = spec.build / "meson-info"
    if not (spec.build / "build.ninja").exists() or not (info / "meson-info.json").exists():
        return "not configured"
    try:
        directories = json.loads((info / "meson-info.json").read_text())["directories"]
        options = {o["name"]: o["value"] for o in json.loads((info / "intro-buildoptions.json").read_text())}
        dependencies = json.loads((info / "intro-dependencies.json").read_text())
    except (OSError, ValueError, KeyError) as error:
        return f"unreadable configuration ({error})"
    if not _same(directories.get("source"), spec.source):
        return f"configured for source {directories.get('source')}"
    if not _same(directories.get("build"), spec.build):
        return f"configured as build directory {directories.get('build')}"
    for name in ("prefix", "pkg_config_path", "cpp_std", "b_ndebug"):
        if name in spec.options and _option_text(options.get(name, "")) != spec.options[name]:
            return f"{name} is {_option_text(options.get(name, ''))}"
    if spec.source.name == "gfxstream":
        prefix = spec.source.parent / "protobuf-prefix"
        for dependency in dependencies:
            if dependency.get("name") == "protobuf":
                paths = _dependency_paths([dependency])
                if not paths or any(not Path(p).is_relative_to(prefix) for p in paths):
                    return "protobuf dependency is outside the coherent prefix"
        marker = spec.build / "anyos-protobuf-receipt.sha256"
        receipt = prefix / "anyos-protobuf-build.json"
        if receipt.exists():
            import hashlib
            expected = hashlib.sha256(receipt.read_bytes()).hexdigest()
            if not marker.exists() or marker.read_text() != expected:
                return "protobuf coherent prefix changed"
    missing = [p for p in _dependency_paths(dependencies) if not os.path.exists(p)]
    if missing:
        return f"dependency paths no longer exist: {missing[:3]}"
    return None


def set_aside_moved_cmake(build: Path, source: Path) -> Path | None:
    """CMake refuses a build directory whose CMakeCache.txt was written for
    another build or source path (MoltenVK's SPIRV-Tools optimizer build after
    the checkout moved).  Set such a directory aside as `.old-<id>` so the
    following configure starts fresh; returns where it went, or None."""
    cache = Path(build) / "CMakeCache.txt"
    if not cache.exists():
        return None
    entries = {}
    for line in cache.read_text(errors="replace").splitlines():
        name, _, value = line.partition("=")
        entries[name.split(":", 1)[0]] = value
    if _same(entries.get("CMAKE_CACHEFILE_DIR"), Path(build)) and _same(entries.get("CMAKE_HOME_DIRECTORY"), Path(source)):
        return None
    aside = Path(build).with_name(Path(build).name + ".old-" + uuid.uuid4().hex)
    Path(build).rename(aside)
    print(f"{build}: configured for {entries.get('CMAKE_CACHEFILE_DIR')}; set aside as {aside.name}", flush=True)
    return aside


def setup_command(spec: BuildDir) -> list[str]:
    return [*spec.meson, "setup", str(spec.build), str(spec.source),
            *(f"-D{name}={value}" for name, value in spec.options.items())]


def environment(spec: BuildDir) -> dict[str, str] | None:
    if spec.source.name == "gfxstream":
        prefix = spec.source.parent / "protobuf-prefix"
        env = {**os.environ, "PATH": str(prefix / "bin") + os.pathsep + os.environ.get("PATH", ""),
                "PKG_CONFIG_LIBDIR": str(prefix / "lib/pkgconfig"),
                "CC": subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip(),
                "CXX": subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip(),
                "SDKROOT": subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip(),
                "MACOSX_DEPLOYMENT_TARGET": "11.0"}
        for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "CPLUS_INCLUDE_PATH",
                    "LIBRARY_PATH", "PKG_CONFIG_PATH", "CMAKE_PREFIX_PATH"):
            env.pop(key, None)
        return env
    if not spec.path:
        return None
    return {**os.environ, "PATH": os.pathsep.join([*map(str, spec.path), os.environ.get("PATH", "")])}


def _default_run(command: list[str], env: dict | None, log: Path | None) -> None:
    print("+", " ".join(command), flush=True)
    output = log.open("w") if log else None
    try:
        # Like every build command under a writer lease: the child inherits the
        # lease descriptors, so the lease outlives an interrupted parent.
        result = artifact_lease.run(command, env=env, stdout=output, stderr=subprocess.STDOUT if output else None)
    finally:
        if output:
            output.close()
    if result.returncode:
        raise SystemExit(f"meson setup failed ({result.returncode})" + (f"; see {log}" if log else ""))


def ensure(name: str, stage: Path, product: Path, *, log: Path | None = None,
           run: Callable[[list[str], dict | None, Path | None], None] = _default_run) -> Path | None:
    """Configure `name`'s build directory for the current paths when it is not.
    Returns where a replaced directory was set aside, or None."""
    spec = build_dirs(stage, product)[name]
    if name == "gfxstream" and run is _default_run:
        protobuf = _protobuf_module()
        reason = protobuf.cache_problem(Path(stage) / "protobuf-prefix", protobuf.identity(Path(stage) / "protobuf-prefix"))
        if reason:
            raise SystemExit(f"{reason}; run scripts/build-gpu-protobuf.py --stage {stage}")
    for directory in pkgconfig_dirs(spec):
        for pc in relocate_pkgconfig(directory):
            print(f"{name}: {pc} now names prefix {directory.parent.parent}", flush=True)
    reason = problem(spec)
    if reason is None:
        return None
    print(f"{name}: configuring {spec.build} ({reason})", flush=True)
    aside = None
    if spec.build.exists() or spec.build.is_symlink():
        aside = spec.build.with_name(spec.build.name + ".old-" + uuid.uuid4().hex)
        spec.build.rename(aside)
    run(setup_command(spec), environment(spec), log)
    if name == "gfxstream":
        receipt = Path(stage) / "protobuf-prefix/anyos-protobuf-build.json"
        if receipt.exists():
            import hashlib
            (spec.build / "anyos-protobuf-receipt.sha256").write_text(hashlib.sha256(receipt.read_bytes()).hexdigest())
    if problem(spec) is not None:
        raise SystemExit(f"{name}: {spec.build} still not configured for this checkout: {problem(spec)}")
    return aside


def _protobuf_module():
    spec = importlib.util.spec_from_file_location("gpu_protobuf", ROOT / "scripts/build-gpu-protobuf.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def prepare_protobuf(stage: Path, jobs: int = 4) -> Path:
    return _protobuf_module().ensure(stage, jobs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", type=Path, default=ROOT / ".runtime/gfxstream-stage1")
    parser.add_argument("--product", type=Path, default=ROOT / ".runtime/gpu-build")
    parser.add_argument("components", nargs="+")
    args = parser.parse_args()
    known = build_dirs(args.stage, args.product)
    unknown = [n for n in args.components if n not in known]
    if unknown:
        parser.error(f"unknown components {unknown}; known: {list(known)}")
    stage, product = args.stage.resolve(), args.product.resolve()
    slots = list(dict.fromkeys(slot for name in args.components for slot in lease_slots(name, stage, product)))
    if "gfxstream" in args.components:
        prepare_protobuf(stage)
    with artifact_lease.leases(write=slots):
        for name in args.components:
            ensure(name, stage, product)


if __name__ == "__main__":
    main()
