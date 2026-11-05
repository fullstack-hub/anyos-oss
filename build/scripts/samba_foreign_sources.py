"""Download the exact Samba foreign-library sources and Homebrew patches.

The manifest and formula snapshots travel with the release source bundle.
Downloads are verified before export; an existing cache is never trusted blindly.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def download(entry, cache):
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / entry['filename']
    if target.exists():
        if hashlib.sha256(target.read_bytes()).hexdigest() != entry['sha256']:
            raise SystemExit(f'foreign source checksum mismatch: {target}')
        return target
    for url in [entry['url'], *entry.get('mirrors', [])]:
        with tempfile.TemporaryDirectory(dir=cache) as temp:
            candidate = Path(temp) / 'download'
            result = subprocess.run(['curl', '--fail', '--location', '--retry', '2',
                                     '--connect-timeout', '20', '--max-time', '300',
                                     '--silent', '--show-error', '--output', str(candidate), url])
            if result.returncode:
                continue
            if hashlib.sha256(candidate.read_bytes()).hexdigest() != entry['sha256']:
                raise SystemExit(f'foreign source checksum mismatch: {url}')
            shutil.move(candidate, target)
            return target
    raise SystemExit(f'cannot download foreign source: {entry["url"]}')


def export_sources(resources, cache, destination):
    manifest = json.loads((resources / 'foreign-sources.json').read_text())
    destination.mkdir(parents=True)
    shutil.copy2(resources / 'foreign-sources.json', destination)
    for component in manifest['components']:
        directory = destination / component['name']
        directory.mkdir()
        for entry in [component, *component['patches']]:
            shutil.copy2(download(entry, cache), directory / entry['filename'])
        formula = resources / component['formula']
        recipe = destination / component['formula']
        recipe.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(formula, recipe)


def verify_libraries(samba, origins, manifest_path):
    """Refuse packaging a different keg/version or an unrecorded dependency.

    origins maps staged foreign paths to their original resolved Cellar paths.
    Only libraries retained after reachability pruning are shipped.
    """
    manifest = json.loads(manifest_path.read_text())
    components = {c['name']: c for c in manifest['components']}
    for target in (samba / 'lib/foreign').iterdir():
        origin = origins[target]
        parts = origin.parts
        try:
            index = parts.index('Cellar')
            name, version = parts[index + 1:index + 3]
            component = components[name]
        except (ValueError, KeyError):
            raise RuntimeError(f'unrecorded Samba foreign library: {origin}')
        if version != component['version'] or target.name not in component['libraries']:
            raise RuntimeError(f'Samba foreign library differs from source pin: {origin}')
