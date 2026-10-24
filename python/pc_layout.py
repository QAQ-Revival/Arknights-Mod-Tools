"""PC resource paths, with downloaded bundles taking precedence over install data."""
import json
import re
from functools import lru_cache
from pathlib import Path, PurePosixPath

RESOURCE_MANIFEST = '.qaqr-resources.json'
CHARACTER = re.compile(r'char_\d+_[a-z0-9]+', re.I)
CHARACTER_DIRS = ('chararts', 'skinpack', 'charpack', 'arts/dynchars', 'refs/fx/texture/overseas')
SHARED_BUNDLES = ('arts/charavatars/avatar_hub.ab', 'arts/charportraits/portraits_hub.ab', 'arts/dynchars/effect.ab')


def relative_path(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value or any(ord(c) < 32 for c in value):
        raise ValueError('Invalid resource-relative path')
    parts = value.split('/')
    if any(part in ('', '.', '..') or part.endswith((' ', '.')) for part in parts):
        raise ValueError('Invalid resource-relative path')
    return PurePosixPath(value).as_posix()


def checked_file(root, relative):
    file = root / relative_path(relative)
    current = file
    while current != root:
        if current.is_symlink() or (hasattr(current, 'is_junction') and current.is_junction()):
            raise ValueError('Linked resource is not supported')
        current = current.parent
    if not file.resolve().is_relative_to(root.resolve()):
        raise ValueError('Resource escaped its root')
    return file


def game_roots(game_path):
    if not game_path:
        return []
    exe = Path(game_path).resolve()
    if exe.name.lower() != 'arknights.exe' or not exe.is_file():
        raise ValueError('Select Arknights.exe')
    data = exe.parent / 'Arknights_Data'
    return [data / 'PersistentData/Bundles', data / 'StreamingAssets/AB/Windows']


def character_hints(request):
    values = request.get('characterIds', [])
    if not isinstance(values, list) or len(values) > 1000 or any(not isinstance(value, str) or not CHARACTER.fullmatch(value) for value in values):
        raise ValueError('Invalid operator identities')
    return {value.lower() for value in values}


@lru_cache(maxsize=4)
def _bundle_index(root_values):
    # Keep complete logical paths; a basename alone may identify audio, art and
    # battle packages simultaneously. PersistentData wins for the same path.
    found = {}
    for value in root_values:
        root = Path(value)
        if root.is_dir():
            for file in root.rglob('*.ab'):
                relative = file.relative_to(root).as_posix()
                found.setdefault(relative.casefold(), (relative, file))
    return tuple(found.values())


def requested_bundles(roots, requested):
    found = {}
    unresolved = []
    for value in requested:
        if not isinstance(value, str) or not value.lower().endswith('.ab'):
            continue
        value = relative_path(value)
        for root in roots:
            file = checked_file(root, value)
            if file.is_file():
                found[value.casefold()] = (value, file)
                break
        else:
            unresolved.append(value)
    if not unresolved:
        return list(found.values())
    indexed = _bundle_index(tuple(str(root) for root in roots))
    by_name = {}
    for relative, file in indexed:
        by_name.setdefault(file.name.casefold(), []).append((relative, file))
    for value in unresolved:
        if not isinstance(value, str) or not value.lower().endswith('.ab'):
            continue
        value = relative_path(value).casefold()
        candidates = by_name.get(PurePosixPath(value).name, [])
        exact = [item for item in candidates if value == item[0].casefold() or value.endswith('/' + item[0].casefold())]
        if len(exact) == 1:
            candidates = exact
        elif len(candidates) > 1:
            parts = value.split('/')
            def score(item):
                other = item[0].casefold().split('/')
                count = 0
                for a, b in zip(reversed(parts), reversed(other)):
                    if a != b: break
                    count += 1
                return count
            best = max(map(score, candidates))
            candidates = [item for item in candidates if score(item) == best]
        if len(candidates) == 1:
            relative, file = candidates[0]
            root = next(root for root in roots if file.is_relative_to(root))
            checked_file(root, relative)
            found[relative.casefold()] = (relative, file)
    return list(found.values())


def current_bundles(roots, ids, requested=()):
    """Return one physical source for each full logical bundle path."""
    ids = {value.lower() for value in ids}
    requested = set(requested)
    found = {relative.casefold(): (relative, file) for relative, file in requested_bundles(roots, requested)}
    for root in roots:
        for category in CHARACTER_DIRS:
            directory = checked_file(root, category)
            if not directory.is_dir():
                continue
            iterator = directory.rglob('*.ab') if category.startswith('refs/') else directory.glob('*.ab')
            for file in iterator:
                if not ids.intersection(match.lower() for match in CHARACTER.findall(file.stem)):
                    continue
                relative = file.relative_to(root).as_posix()
                checked_file(root, relative)
                found.setdefault(relative.lower(), (relative, file))
        for relative in SHARED_BUNDLES:
            prefix = 'charavatars_' if 'charavatars' in relative else 'charportraits_' if 'charportraits' in relative else 'effect__texture_'
            if relative not in requested and PurePosixPath(relative).name not in requested and not any(value.startswith(prefix) for value in requested):
                continue
            file = checked_file(root, relative)
            if file.is_file():
                found.setdefault(relative.lower(), (relative, file))
    return [found[key] for key in sorted(found)]


def read_bindings(root, *, max_manifests=1000):
    """Read metadata next to each imported Mod, including deployed Mod subfolders."""
    bindings = {}
    manifests = sorted(root.rglob(RESOURCE_MANIFEST))
    if len(manifests) > max_manifests:
        raise ValueError('Too many resource manifests')
    for manifest in manifests:
        if any(part.lower().startswith(('disable_', 'disabled_')) for part in manifest.relative_to(root).parts[:-1]):
            continue
        checked_file(root, manifest.relative_to(root).as_posix())
        if manifest.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('Resource manifest is too large')
        data = json.loads(manifest.read_text(encoding='utf-8'))
        if data.get('schemaVersion') != 1 or not isinstance(data.get('resources'), list) or len(data['resources']) > 20000:
            raise ValueError('Invalid resource manifest')
        for item in data['resources']:
            file = checked_file(manifest.parent, item['file'])
            bundle = relative_path(item['bundle'])
            target = relative_path(item['target'])
            if '/' in target or not bundle.lower().endswith('.ab'):
                raise ValueError('Invalid resource binding')
            if not file.is_file() or file in bindings:
                raise ValueError('Missing or duplicate resource binding')
            bindings[file] = {'bundle': bundle, 'target': target}
    return bindings
