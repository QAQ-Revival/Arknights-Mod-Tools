"""Rebuild external PC bundles from current local originals, without game writes."""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
from functools import lru_cache
from pathlib import Path

import UnityPy
from PIL import Image
from UnityPy.enums import TextureFormat
from worker import BUNDLE_EXTS, bundle_records, normalized, source_files
from pc_layout import CHARACTER, character_hints, checked_file, current_bundles, game_roots, read_bindings, relative_path

MAX_BUNDLES = 4096
PARTIAL_INDEX = 'bundle-index.partial.jsonl'


def digest(file):
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def report(phase, done, total):
    """One line per bundle on stderr: the caller only stops a build that falls silent."""
    try:
        sys.stderr.write('\t'.join(('QAQR-PROGRESS', phase, str(done), str(total))) + '\n')
        sys.stderr.flush()
    except (AttributeError, OSError, ValueError):
        pass  # The caller is gone. Finish anyway: the next build takes these bundles over.


def read_cache(directories):
    """Bundles of earlier builds by logical path. A finished build has an index;
    an interrupted one lists each bundle it finished on a line of its own."""
    cache = {}
    for directory in directories:
        root = Path(directory)
        try:
            data = json.loads((root / 'bundle-index.json').read_text(encoding='utf-8'))
            entries = data['bundles'] if data['schemaVersion'] == 1 else []
        except (OSError, ValueError, KeyError, TypeError):
            entries = []
            try:
                lines = (root / PARTIAL_INDEX).read_text(encoding='utf-8').splitlines()
            except (OSError, ValueError):
                lines = []
            for line in lines:
                try:
                    entries.append(json.loads(line))
                except ValueError:
                    pass  # The build stopped while writing this line.
        for entry in entries if isinstance(entries, list) else []:
            if isinstance(entry, dict) and isinstance(entry.get('logical'), str):
                cache.setdefault(entry['logical'].casefold(), []).append((entry, root))
    return cache


def input_fingerprints(items, source, hash_file, approved_pairs):
    """The fingerprint of one bundle's inputs, then the one older builds stored.
    A conflict approval concerns the bundle both of its files go into; older
    builds counted every approval, so confirming one rebuilt every bundle."""
    inputs = [(item['file'].relative_to(source).as_posix(), item['key'], hash_file(item['file'])) for item in items]
    names = {name for name, _, _ in inputs}
    def fingerprint(pairs):
        return hashlib.sha256(json.dumps([inputs, pairs], ensure_ascii=False).encode('utf-8')).hexdigest()
    return fingerprint(sorted(pair for pair in approved_pairs if all(name in names for name, _ in pair))), fingerprint(sorted(approved_pairs))


def input_fingerprint(items, source, hash_file, approved_pairs):
    return input_fingerprints(items, source, hash_file, approved_pairs)[0]


def cached_bundle(cache, relative, original_hash, fingerprints):
    for entry, directory in cache.get(relative.casefold(), []):
        try:
            if entry.get('inputFingerprint') not in fingerprints or entry['sourceSha256'] != original_hash:
                continue
            file = checked_file(directory, entry['replacement'])
            if file.is_file() and digest(file) == entry['replacementSha256']:
                return {**entry, 'inputFingerprint': fingerprints[0]}, file
        except (KeyError, TypeError, ValueError, OSError):
            continue  # An unusable entry is rebuilt like a missing one.
    return None


def build_bundles(request):
    source = Path(request["source"]).resolve(strict=True)
    roots = game_roots(request.get("gamePath"))
    if not roots:
        raise ValueError("Select the current Arknights.exe before preparing PC bundles")
    game = Path(request["gamePath"]).resolve(strict=True)
    output = Path(request["output"]).resolve()
    if output.exists() or output.is_relative_to(source) or source.is_relative_to(output) or output.is_relative_to(game.parent):
        raise ValueError("Output must be new and outside the game and Mod library")
    inputs = source_files(source, max_files=100000)
    # A user confirmation approves exact resource pairs, including their bytes.
    # Changed files and ordinary standalone conversions still reject overlaps.
    marker = source / '.qaqr-m4a.json'
    deployment = json.loads(marker.read_text(encoding='utf-8')) if marker.is_file() else {}
    approved_pairs = set()
    if deployment.get('owner') == 'qaqr-arknights-v1':
        approved_pairs = {tuple(sorted((item['file'], item['sha256']) for item in pair)) for pair in deployment.get('approvedResourcePairs', [])}
        priority = {name: index for index, name in enumerate(deployment.get('modOrder', []))}
        inputs.sort(key=lambda file: (priority.get(file.relative_to(source).parts[0], -1), file.as_posix()))
    bindings = read_bindings(source, max_manifests=10000)
    resources = []
    by_key = {}
    for file in inputs:
        if file.suffix.lower() in BUNDLE_EXTS:
            raise ValueError("Convert raw Mod bundles into resources before enabling them")
        if file.suffix.lower() not in (".png", ".skel", ".atlas", ".json"):
            raise ValueError("Source loader supports character PNG/Spine resources only: " + file.name)
        binding = bindings.get(file, {})
        resources.append({'file': file, 'key': normalized(binding.get('target', file.name)), 'bundle': binding.get('bundle')})
        by_key.setdefault(resources[-1]['key'], []).append(resources[-1])
    hash_resource = lru_cache(maxsize=None)(digest)
    fully_bound = all(item['bundle'] for item in resources)
    position = {item['file']: index for index, item in enumerate(resources)}
    by_bundle = {}
    for item in resources:
        if item['bundle']:
            by_bundle.setdefault(item['bundle'].casefold(), []).append(item)
    # resumeDirectories are what interrupted builds left behind. A caller that
    # passes the list, even empty, also gets this build's work kept on failure.
    resumable = 'resumeDirectories' in request
    resume_directories = request.get('resumeDirectories', [])
    if not isinstance(resume_directories, list) or len(resume_directories) > 64 or any(not isinstance(value, str) or not value for value in resume_directories):
        raise ValueError('Invalid resume directory list')
    cache = read_cache([*resume_directories, *([request['cacheDirectory']] if request.get('cacheDirectory') else [])])
    ids = {match.lower() for file in inputs for match in CHARACTER.findall(file.name)}
    ids.update(character_hints(request))
    ids.update(match.lower() for item in bindings.values() for match in CHARACTER.findall(item['bundle']))
    if len(ids) > 1000:
        raise ValueError("Too many operators in a single preparation")
    requested = [item['bundle'] or item['key'] for item in resources]
    # One patch may cover several toggled Mods (scopeSources); each must bind
    # unambiguously, or the caller retries them as separate patches.
    scopes = request.get('scopeSources')
    if scopes is None and request.get('scopeSource') is not None:
        scopes = [request['scopeSource']]
    scoped = scopes is not None
    affected = set()
    if scoped:
        if not isinstance(scopes, list) or not scopes or len(scopes) > 1000:
            raise ValueError('Invalid scope list')
        previous = request.get('affectedBundles', [])
        if not isinstance(previous, list) or len(previous) > MAX_BUNDLES:
            raise ValueError('Invalid affected bundle list')
        affected.update(relative_path(value).casefold() for value in previous)
        for scope_source in scopes:
            scope = Path(scope_source).resolve(strict=True)
            scope_bindings = read_bindings(scope)
            scope_files = [file for file in source_files(scope) if file.suffix.lower() in ('.png', '.skel', '.atlas', '.json')]
            scope_items = [(normalized(scope_bindings.get(file, {}).get('target', file.name)), scope_bindings.get(file, {}).get('bundle')) for file in scope_files]
            scope_ids = {match.lower() for file in scope_files for match in CHARACTER.findall(file.name)}
            scope_ids.update(character_hints(request))
            scope_requested = [bundle or key for key, bundle in scope_items]
            scope_matches = {item: set() for item in scope_items}
            if scope_items and all(bundle for _, bundle in scope_items):
                for item in scope_items:
                    scope_matches[item].add(item[1].casefold())
            else:
                for relative, original in current_bundles(roots, scope_ids, scope_requested):
                    records, _, _ = bundle_records(original)
                    keys = {normalized(record['target']) for record in records}
                    for item in scope_items:
                        key, bundle = item
                        if key in keys and (not bundle or bundle.casefold() == relative.casefold()):
                            scope_matches[item].add(relative.casefold())
            if any(len(matches) != 1 for matches in scope_matches.values()):
                raise ValueError('The selected Mod needs unambiguous current PC resource bindings; diagnose and convert it first: ' + scope.name)
            for matches in scope_matches.values():
                affected.update(matches)
        if len(affected) > MAX_BUNDLES:
            raise ValueError('Too many bundles in a single patch')
        requested = sorted(affected)
    loaded = []
    reused = []
    fingerprints = {}
    matches = {item['file']: [] for item in resources if not scoped or (item['bundle'] and item['bundle'].casefold() in affected)}
    removed = []
    selected_originals = current_bundles(roots, set() if scoped or (resources and all(item['bundle'] for item in resources)) else ids, requested)
    if len(selected_originals) > MAX_BUNDLES:
        raise ValueError('Too many bundles in a single preparation')
    if scoped and affected.difference(relative.casefold() for relative, _ in selected_originals):
        raise ValueError('A previously loaded game bundle is missing; restart and prepare Mods against the current game version')
    def select_inputs(relative, original):
        records, _, _ = bundle_records(original)
        selected, matched = [], {}
        for record in records:
            for item in by_key.get(normalized(record['target']), []):
                if not item['bundle'] or item['bundle'].casefold() == relative.casefold():
                    selected.append((record['target'], item['file']))
                    matches.setdefault(item['file'], []).append((relative, record['target']))
                    matched[item['file']] = item
        return selected, sorted(matched.values(), key=lambda item: position[item['file']])
    for index, (relative, original) in enumerate(selected_originals):
        if resumable:
            report('scan', index, len(selected_originals))
        before_hash = digest(original)
        items = by_bundle.get(relative.casefold(), [])
        selected = None
        if not fully_bound:
            # A resource without a binding goes to the bundle that has its name,
            # so the bundle is read to learn its inputs. It is still only rebuilt
            # when those inputs changed.
            selected, items = select_inputs(relative, original)
        fingerprints[relative] = input_fingerprints(items, source, hash_resource, approved_pairs)
        cached = cached_bundle(cache, relative, before_hash, fingerprints[relative])
        if cached:
            entry, file = cached
            reused.append(({**entry, 'source': original.relative_to(game.parent).as_posix()}, file))
            if fully_bound:
                for item in items:
                    matches[item['file']].append((relative, item['key']))
            continue
        if fully_bound:
            if scoped and not items:
                removed.append({'source': original.relative_to(game.parent).as_posix(), 'sourceSha256': before_hash, 'logical': relative})
                continue
            selected, _ = select_inputs(relative, original)
        if selected:
            loaded.append((relative, original, before_hash, selected))
        elif scoped:
            removed.append({'source': original.relative_to(game.parent).as_posix(), 'sourceSha256': before_hash, 'logical': relative})
    missing = [file.name for file, records in matches.items() if not records]
    ambiguous = [file.name for file, records in matches.items() if len(records) > 1]
    if missing:
        raise ValueError('No unambiguous current PC binding for: ' + ', '.join(missing[:8]))
    if ambiguous:
        raise ValueError('Specify a bundle in the resource manifest for: ' + ', '.join(ambiguous[:8]))
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.qaqr-bundles-', dir=output.parent))
    built = False
    partial = (stage / PARTIAL_INDEX).open('w', encoding='utf-8') if resumable else None
    try:
        bundles = []
        def finished(entry):
            # One line per finished bundle lets the next build take over from here.
            bundles.append(entry)
            if partial:
                partial.write(json.dumps(entry, ensure_ascii=False) + '\n')
                partial.flush()
        for index, (entry, file) in enumerate(reused):
            if resumable and index % 64 == 0:
                report('reuse', index, len(reused))
            target = stage / entry['replacement']
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(file, target)
            except OSError:
                shutil.copyfile(file, target)
            finished(entry)
        for index, (relative, original, before_hash, targets) in enumerate(loaded):
            if resumable:
                report('build', index, len(loaded))
            if digest(original) != before_hash:
                raise ValueError('Game resources changed during preparation')
            records, _, platforms, env = bundle_records(original, return_env=True)
            by_target = {record['target']: record for record in records}
            selected = [(by_target[target], file) for target, file in targets]
            if any(platform not in (5, 19) for platform in platforms):
                raise ValueError("The selected game has non-Windows bundle resources")
            before_ids = sorted((obj.path_id, obj.type.name) for obj in env.objects)
            identity = lambda obj: (obj.assets_file.name, obj.path_id)
            selected_ids = {identity(record['object']) for record, _ in selected}
            preserved = {identity(obj): hashlib.sha256(obj.get_raw_data()).digest() for obj in env.objects if identity(obj) not in selected_ids}
            changed = []
            written = {}
            for record, file in selected:
                oid = identity(record['object'])
                file_hash = hash_resource(file)
                if oid in written:
                    previous_file, previous_hash = written[oid]
                    pair = tuple(sorted(((previous_file, previous_hash), (file.relative_to(source).as_posix(), file_hash))))
                    if previous_hash != file_hash and pair not in approved_pairs:
                        raise ValueError('Conflicting modifications to the same PC object: ' + relative + ' / ' + record['target'])
                    if previous_hash == file_hash:
                        continue
                written[oid] = (file.relative_to(source).as_posix(), file_hash)
                data = record["object"].read()
                if record["kind"] == "texture":
                    with Image.open(file) as image:
                        if image.width * image.height > 64 * 1024 * 1024:
                            raise ValueError("Texture is too large")
                        data.set_image(image.convert("RGBA"), target_format=TextureFormat.RGBA32)
                elif record["kind"] == "spine":
                    data.m_Script = file.read_bytes().decode("utf-8", "surrogateescape")
                else:
                    raise ValueError("Unsupported resource binding")
                data.save()
                changed.append(record["target"])
            if len(env.files) != 1:
                raise ValueError("Unexpected bundle root layout")
            encoded = next(iter(env.files.values())).save(packer="lz4")
            # Reparse every generated package and preserve the PC object graph.
            checked = UnityPy.load(encoded)
            if sorted((obj.path_id, obj.type.name) for obj in checked.objects) != before_ids:
                raise ValueError("Generated bundle object identity changed")
            if sorted({int(asset.target_platform) for asset in checked.assets}) != platforms:
                raise ValueError("Generated bundle platform changed")
            checked_objects = {identity(obj): obj for obj in checked.objects}
            for oid, expected in preserved.items():
                if hashlib.sha256(checked_objects[oid].get_raw_data()).digest() != expected:
                    raise ValueError("An unrelated PC object changed during serialization")
            for record, file in selected:
                actual = checked_objects[identity(record["object"])].read()
                if record["kind"] == "texture":
                    with Image.open(file) as expected:
                        if actual.image.convert("RGBA").tobytes() != expected.convert("RGBA").tobytes():
                            raise ValueError("Texture pixel round-trip check failed")
                elif actual.m_Script.encode("utf-8", "surrogateescape") != file.read_bytes():
                    raise ValueError("Spine byte round-trip check failed")
            if digest(original) != before_hash:
                raise ValueError("Game resources changed during preparation")
            replacement = 'bundles/' + relative
            target = stage / replacement
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(encoded)
            finished({"source": original.relative_to(game.parent).as_posix(), "replacement": replacement, "sourceSha256": before_hash, "replacementSha256": hashlib.sha256(encoded).hexdigest(), "resources": changed, "logical": relative, "inputs": sorted({file.relative_to(source).as_posix() for _, file in targets}), "inputFingerprint": fingerprints[relative][0]})
            built = True
        lines = ["QAQR-ARKNIGHTS-PATCH\t1" if scoped else "QAQR-ARKNIGHTS-BUNDLES\t1", "GAME\t" + str(game)]
        for bundle in bundles:
            fields = ["BUNDLE", bundle["source"], bundle["replacement"], bundle["sourceSha256"], bundle["replacementSha256"]]
            if any("\t" in value or "\n" in value or "\r" in value for value in fields):
                raise ValueError("Invalid manifest field")
            lines.append("\t".join(fields))
        for bundle in removed:
            lines.append('\t'.join(['REMOVE', bundle['source'], bundle['sourceSha256']]))
        (stage / "manifest.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
        if partial:
            partial.close()
        stage.rename(output)
        return {"success": True, "bundleCount": len(bundles), "reusedBundleCount": len(reused), "resourceCount": len(matches), "bundles": bundles, "removedBundles": removed, "objectAndResourceRoundTripVerified": True, "runtimeValidated": False}
    except BaseException:
        if partial:
            partial.close()
        # A resumable build keeps the bundles it finished for the next attempt.
        if not (resumable and built):
            shutil.rmtree(stage, ignore_errors=True)
        raise
