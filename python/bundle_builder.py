"""Rebuild external PC bundles from current local originals, without game writes."""
import hashlib
import io
import tempfile
from pathlib import Path

import UnityPy
from PIL import Image
from UnityPy.enums import TextureFormat
from worker import BUNDLE_EXTS, bundle_records, normalized, source_files
from pc_layout import CHARACTER, character_hints, current_bundles, game_roots, read_bindings, relative_path


def digest(file):
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


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
    ids = {match.lower() for file in inputs for match in CHARACTER.findall(file.name)}
    ids.update(character_hints(request))
    ids.update(match.lower() for item in bindings.values() for match in CHARACTER.findall(item['bundle']))
    if len(ids) > 1000:
        raise ValueError("Too many operators in a single preparation")
    requested = [item['bundle'] or item['key'] for item in resources]
    scoped = request.get('scopeSource') is not None
    affected = set()
    if scoped:
        previous = request.get('affectedBundles', [])
        if not isinstance(previous, list) or len(previous) > 200:
            raise ValueError('Invalid affected bundle list')
        affected.update(relative_path(value).casefold() for value in previous)
        scope = Path(request['scopeSource']).resolve(strict=True)
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
            raise ValueError('The selected Mod needs unambiguous current PC resource bindings; diagnose and convert it first')
        for matches in scope_matches.values():
            affected.update(matches)
        requested = sorted(affected)
    loaded = []
    matches = {item['file']: [] for item in resources if not scoped or (item['bundle'] and item['bundle'].casefold() in affected)}
    removed = []
    selected_originals = current_bundles(roots, set() if scoped or (resources and all(item['bundle'] for item in resources)) else ids, requested)
    if scoped and affected.difference(relative.casefold() for relative, _ in selected_originals):
        raise ValueError('A previously loaded game bundle is missing; restart and prepare Mods against the current game version')
    for relative, original in selected_originals:
        before_hash = digest(original)
        records, _, platforms, env = bundle_records(original, return_env=True)
        selected = []
        for record in records:
            for item in by_key.get(normalized(record['target']), []):
                if normalized(record['target']) == item['key'] and (not item['bundle'] or item['bundle'].casefold() == relative.casefold()):
                    selected.append((record['target'], item['file']))
                    matches.setdefault(item['file'], []).append((relative, record['target']))
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
    with tempfile.TemporaryDirectory(prefix='.qaqr-bundles-', dir=output.parent) as temporary:
        stage = Path(temporary)
        bundles = []
        for relative, original, before_hash, targets in loaded:
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
                file_hash = digest(file)
                if oid in written:
                    if written[oid] != file_hash:
                        raise ValueError('Conflicting modifications to the same PC object: ' + relative + ' / ' + record['target'])
                    continue
                written[oid] = file_hash
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
            bundles.append({"source": original.relative_to(game.parent).as_posix(), "replacement": replacement, "sourceSha256": before_hash, "replacementSha256": hashlib.sha256(encoded).hexdigest(), "resources": changed, "logical": relative, "inputs": sorted({file.relative_to(source).as_posix() for _, file in targets})})
            target = stage / replacement
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(encoded)
        lines = ["QAQR-ARKNIGHTS-PATCH\t1" if scoped else "QAQR-ARKNIGHTS-BUNDLES\t1", "GAME\t" + str(game)]
        for bundle in bundles:
            fields = ["BUNDLE", bundle["source"], bundle["replacement"], bundle["sourceSha256"], bundle["replacementSha256"]]
            if any("\t" in value or "\n" in value or "\r" in value for value in fields):
                raise ValueError("Invalid manifest field")
            lines.append("\t".join(fields))
        for bundle in removed:
            lines.append('\t'.join(['REMOVE', bundle['source'], bundle['sourceSha256']]))
        (stage / "manifest.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
        stage.rename(output)
        return {"success": True, "bundleCount": len(bundles), "resourceCount": len(matches), "bundles": bundles, "removedBundles": removed, "objectAndResourceRoundTripVerified": True, "runtimeValidated": False}
