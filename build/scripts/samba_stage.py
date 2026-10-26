"""Stage a relocatable Samba installation, shared by packaging and source rebuilds.

No Any OS engine, signing configuration or application sources are required.
"""
from pathlib import Path
import argparse
import re
import shutil
import stat
import subprocess

from samba_foreign_sources import verify_libraries


def run(*args):
    result = subprocess.run([str(a) for a in args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed: {result.stderr or result.stdout}')
    return result.stdout


MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe"}


def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as handle:
        return handle.read(4) in MACHO_MAGICS


def machos_under(directory: Path) -> list[Path]:
    return sorted(p for p in directory.rglob("*") if is_macho(p))


def writable(path: Path) -> None:
    path.chmod(path.stat().st_mode | stat.S_IWUSR)


def dependencies(binary: Path) -> list[str]:
    """Load commands' dependency names, without the binary's own install name."""
    lines = run("otool", "-L", binary).splitlines()[1:]
    names = [line.strip().split(" (", 1)[0] for line in lines if line.strip()]
    own = install_name(binary)
    return [n for n in names if n != own]


def install_name(binary: Path) -> str | None:
    lines = run("otool", "-D", binary).splitlines()
    return lines[1].strip() if len(lines) > 1 and lines[1].strip() else None


def rpaths(binary: Path) -> list[str]:
    lines = run("otool", "-l", binary).splitlines()
    found: list[str] = []
    for index, line in enumerate(lines[:-2]):
        if line.strip() == "cmd LC_RPATH":
            value = lines[index + 2].strip()
            if value.startswith("path "):
                found.append(value[5:].split(" (offset ", 1)[0])
    return found



# Loadable modules under lib/, found by name at run time (smb.conf).
MODULE_DIRS = ("vfs", "idmap", "ldb", "krb5", "nss_info", "auth", "pdb", "charset", "gpext", "perfcount", "rpc")


def stage_samba(stage_root: Path, prefix: Path, build_tree: Path,
                brew_sources: list[str] | None = None, log=print) -> Path | None:
    """The relocatable Samba build (scripts/build-samba.sh) as
    `samba/{sbin,bin,lib,share}` with every Mach-O relinked to the tree:
    Samba's own libraries and modules keep their `lib/…` layout and are
    referenced as `@rpath/<path under lib>`, foreign (Homebrew) libraries
    are closed over into `lib/foreign/`, and the run-path of each binary
    is its own way back to `lib/` (`@executable_path/../lib` for the
    programs, `@loader_path/..` and `@loader_path/../..` for libraries and
    modules). The core names the tree's directories to smbd through the
    environment (Samba patch 0001). Missing build: the app reports shared
    folders as unavailable."""
    if not (prefix / "sbin/smbd").is_file():
        log("samba: not built (scripts/build-samba.sh); shared folders will be unavailable in the package")
        return None
    if brew_sources is None:
        brew_sources = []
    samba = stage_root / "samba"
    if samba.exists():
        raise FileExistsError(f"Samba destination already exists: {samba}")
    log(f"samba: {prefix} -> {samba}")
    for relative in ("sbin/smbd", "bin/pdbedit", "bin/testparm", "anyos-samba.json"):
        source = prefix / relative
        if source.exists():
            (samba / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, samba / relative)
    # The DCE/RPC helpers smbd spawns (share enumeration, the registry,
    # ...); the core points smbd at them (SAMBA_DYN_SAMBA_LIBEXECDIR).
    shutil.copytree(prefix / "libexec/samba", samba / "libexec/samba", symlinks=False)
    # lib/ holds each library once under its full version (libfoo.1.0.0)
    # plus the shorter names as links; everything gets relinked to the real
    # file, so only that is kept.
    shutil.copytree(prefix / "lib", samba / "lib", symlinks=True)
    lib = samba / "lib"
    source_lib = prefix.resolve() / "lib"
    build_tree = build_tree.resolve()
    # A library's installed name -> its real file, relative to lib/.
    installed: dict[str, list[str]] = {}
    for path in sorted(lib.rglob("*")):
        if path.is_symlink() or path.is_file():
            real = path.resolve()
            if real.is_file() and is_macho(real) and lib.resolve() in real.parents:
                relative = real.relative_to(lib.resolve()).as_posix()
                if relative not in installed.setdefault(path.name, []):
                    installed[path.name].append(relative)
    for path in list(lib.rglob("*")):
        if path.is_symlink() or (path.is_file() and not is_macho(path)):
            path.unlink()
    foreign_dir = lib / "foreign"
    foreign_dir.mkdir(exist_ok=True)
    copied: dict[Path, str] = {}  # real foreign file -> name under lib/

    def own_name(dependency: str) -> str | None:
        """`@rpath/<path under lib>` for a dependency inside the Samba tree:
        the build tree Samba's waf links the installed programs against
        (`bin/default/.../libfoo.inst.dylib`, the install flavour of each
        library, which `make install` puts under lib/ as `libfoo.dylib`)
        or the install prefix itself."""
        real = Path(dependency).resolve() if dependency.startswith("/") else None
        if real is None:
            return None
        if source_lib in real.parents:
            name = real.name
        elif build_tree in real.parents:
            name = re.sub(r"\.inst\.dylib$", ".dylib", real.name)
        else:
            return None
        matches = installed.get(name, [])
        if len(matches) != 1:
            raise RuntimeError(f"samba: {dependency}: {len(matches)} installed copies of {name}")
        return matches[0]

    origins: dict[Path, Path] = {}  # staged foreign copy -> the file it came from

    def bring_foreign(source: Path) -> str:
        real = source.resolve()
        if real in copied:
            return copied[real]
        name = f"foreign/{real.name}"
        target = lib / name
        if target.exists():
            raise RuntimeError(f"samba: two different libraries want the name {name}")
        shutil.copy2(real, target)
        writable(target)
        run("install_name_tool", "-id", f"@rpath/{name}", target)
        origins[target] = real
        pending.append(target)
        log(f"  + {name}  <- {real}")
        copied[real] = name
        brew_sources.append(f"samba:{real.name}={real}")
        return name

    def foreign_relative(binary: Path, dependency: str) -> Path:
        """Where a foreign library's `@loader_path/x` or `@rpath/x` points,
        seen from where that library came from (Homebrew's keg)."""
        origin = origins.get(binary)
        if origin is None:
            raise RuntimeError(f"samba: {binary.relative_to(samba)}: unexpected dependency {dependency}")
        if dependency.startswith("@loader_path/"):
            candidate = origin.parent / dependency[len("@loader_path/"):]
            if candidate.exists():
                return candidate
        else:
            leaf = dependency[len("@rpath/"):]
            for entry in rpaths(origin) + ["/opt/homebrew/lib"]:
                if entry.startswith("@loader_path"):
                    candidate = origin.parent / entry[len("@loader_path"):].lstrip("/") / leaf
                elif entry.startswith("@executable_path"):
                    continue
                else:
                    candidate = Path(entry) / leaf
                if candidate.exists():
                    return candidate
        raise RuntimeError(f"samba: {origin}: cannot resolve {dependency}")

    pending = machos_under(samba)
    seen: set[Path] = set()
    while pending:
        binary = pending.pop()
        if binary in seen:
            continue
        seen.add(binary)
        writable(binary)
        relative = binary.relative_to(samba)
        if relative.parts[0] in ("sbin", "bin"):
            back = "@executable_path/../lib"
        elif relative.parts[0] == "libexec":
            back = "@executable_path/../../lib"
        else:
            depth = len(relative.parts) - 2  # parts under lib/, minus the file
            back = "@loader_path/" + "/".join([".."] * depth) if depth > 0 else "@loader_path"
        # One install_name_tool run per binary (a few thousand single
        # changes take minutes).
        changes: list[str] = []
        if relative.parts[0] == "lib":
            changes += ["-id", f"@rpath/{relative.relative_to('lib').as_posix()}"]
        for dependency in dependencies(binary):
            if dependency.startswith(("/usr/lib/", "/System/")):
                continue
            name = own_name(dependency)
            if name is None and dependency.startswith("@rpath/"):
                leaf = dependency[len("@rpath/"):]
                if (lib / leaf).is_file():
                    continue
                matches = installed.get(Path(leaf).name, [])
                if len(matches) == 1:
                    name = matches[0]
                else:
                    name = bring_foreign(foreign_relative(binary, dependency))
            if name is None and dependency.startswith("@loader_path/"):
                name = bring_foreign(foreign_relative(binary, dependency))
            if name is None and dependency.startswith("/"):
                name = bring_foreign(Path(dependency))
            if name is None:
                raise RuntimeError(f"samba: {relative}: unexpected dependency {dependency}")
            changes += ["-change", dependency, f"@rpath/{name}"]
        for entry in dict.fromkeys(rpaths(binary)):
            changes += ["-delete_rpath", entry]
        run("install_name_tool", *changes, "-add_rpath", back, binary)
    # Nothing may still point outside the tree or the system.
    problems = []
    for binary in machos_under(samba):
        for dependency in dependencies(binary):
            if dependency.startswith(("/usr/lib/", "/System/")):
                continue
            if not (dependency.startswith("@rpath/") and (lib / dependency[len("@rpath/"):]).is_file()):
                problems.append(f"{binary.relative_to(samba)}: {dependency}")
    if problems:
        raise RuntimeError("samba is not self-contained:\n  " + "\n  ".join(problems))
    # Libraries none of the programs, helpers or modules reach (smbclient's,
    # the test framework's) are dropped.
    reached: set[Path] = set()
    frontier = [p for p in machos_under(samba) if p.relative_to(samba).parts[0] != "lib" or p.relative_to(lib).parts[0] in MODULE_DIRS]
    while frontier:
        binary = frontier.pop()
        if binary in reached:
            continue
        reached.add(binary)
        for dependency in dependencies(binary):
            if dependency.startswith("@rpath/"):
                frontier.append(lib / dependency[len("@rpath/"):])
    for binary in machos_under(lib):
        if binary not in reached:
            binary.unlink()
    for directory in sorted(lib.rglob("*"), reverse=True):
        if directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()
    verify_libraries(samba, origins, Path(__file__).resolve().parents[1] / 'resources/samba/foreign-sources.json')
    size = sum(p.stat().st_size for p in samba.rglob("*") if p.is_file())
    log(f"samba: {len(reached)} Mach-O files, {size / 1e6:.0f} MB")
    return samba


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--build-tree', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True,
                        help='Creates a new samba/ directory here; existing trees are refused')
    args = parser.parse_args()
    if stage_samba(args.destination.resolve(), args.prefix.resolve(), args.build_tree.resolve()) is None:
        raise SystemExit('Samba prefix missing sbin/smbd')


if __name__ == '__main__':
    main()
