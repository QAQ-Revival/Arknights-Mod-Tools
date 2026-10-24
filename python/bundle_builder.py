"""Rebuild external PC bundles from current local originals, without game writes."""
import hashlib
import io
from pathlib import Path

import UnityPy
from PIL import Image
from UnityPy.enums import TextureFormat
from worker import BUNDLE_EXTS, CHARACTER, CATEGORIES, bundle_records, game_roots, normalized, source_files


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
    inputs = source_files(source)
    resources = {}
    for file in inputs:
        if file.suffix.lower() in BUNDLE_EXTS:
            raise ValueError("Convert raw Mod bundles into resources before enabling them")
        if file.suffix.lower() not in (".png", ".skel", ".atlas", ".json"):
            raise ValueError("Source loader supports character PNG/Spine resources only: " + file.name)
        key = normalized(file.name)
        if key in resources:
            raise ValueError("Conflicting Mod resource: " + file.name)
        resources[key] = file
    ids = {match.lower() for file in inputs for match in CHARACTER.findall(file.name)}
    if len(ids) > 40:
        raise ValueError("Too many operators in a single preparation")
    matched = set()
    bundles = []
    payloads = []
    for char_id in sorted(ids):
        for category in CATEGORIES:
            original = next((root / category / (char_id + ".ab") for root in roots if (root / category / (char_id + ".ab")).is_file()), None)
            if not original:
                continue
            before_hash = digest(original)
            records, _, platforms, env = bundle_records(original, return_env=True)
            selected = [record for record in records if normalized(record["target"]) in resources]
            if not selected:
                continue
            if any(platform not in (5, 19) for platform in platforms):
                raise ValueError("The selected game has non-Windows bundle resources")
            before_ids = sorted((obj.path_id, obj.type.name) for obj in env.objects)
            identity = lambda obj: (obj.assets_file.name, obj.path_id)
            selected_ids = {identity(record["object"]) for record in selected}
            preserved = {identity(obj): hashlib.sha256(obj.get_raw_data()).digest() for obj in env.objects if identity(obj) not in selected_ids}
            changed = []
            for record in selected:
                key = normalized(record["target"])
                file = resources[key]
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
                matched.add(key)
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
            for record in selected:
                file = resources[normalized(record["target"])]
                actual = checked_objects[identity(record["object"])].read()
                if record["kind"] == "texture":
                    with Image.open(file) as expected:
                        if actual.image.convert("RGBA").tobytes() != expected.convert("RGBA").tobytes():
                            raise ValueError("Texture pixel round-trip check failed")
                elif actual.m_Script.encode("utf-8", "surrogateescape") != file.read_bytes():
                    raise ValueError("Spine byte round-trip check failed")
            if digest(original) != before_hash:
                raise ValueError("Game resources changed during preparation")
            relative = "bundles/" + category + "_" + char_id + ".ab"
            bundles.append({"source": original.relative_to(game.parent).as_posix(), "replacement": relative, "sourceSha256": before_hash, "replacementSha256": hashlib.sha256(encoded).hexdigest(), "resources": changed})
            payloads.append((relative, encoded))
    missing = sorted(file.name for key, file in resources.items() if key not in matched)
    if missing:
        raise ValueError("No unambiguous current PC binding for: " + ", ".join(missing[:8]))
    lines = ["QAQR-ARKNIGHTS-BUNDLES\t1", "GAME\t" + str(game)]
    for bundle in bundles:
        fields = ["BUNDLE", bundle["source"], bundle["replacement"], bundle["sourceSha256"], bundle["replacementSha256"]]
        if any("\t" in value or "\n" in value or "\r" in value for value in fields):
            raise ValueError("Invalid manifest field")
        lines.append("\t".join(fields))
    output.mkdir(parents=True)
    for relative, encoded in payloads:
        file = output / relative
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(encoded)
    (output / "manifest.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"success": True, "bundleCount": len(bundles), "resourceCount": len(matched), "bundles": bundles, "objectAndResourceRoundTripVerified": True, "runtimeValidated": False}
