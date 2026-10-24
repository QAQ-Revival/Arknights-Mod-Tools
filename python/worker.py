"""Read Unity bundles and export M4A resources. Never writes to the game or source.

Bindings come from AssetBundle preloads and Spine object references, not object
order: front/back textures frequently have exactly the same m_Name.
"""
import contextlib
import hashlib
import io
import json
import re
import sys
from pathlib import Path

import UnityPy
from UnityPy.enums import CompressionFlags
from UnityPy.helpers import CompressionHelper
from lz4ak import decompress_lz4ak

CompressionHelper.DECOMPRESSION_MAP[CompressionFlags.LZHAM] = decompress_lz4ak
MAX_BUNDLE = 512 * 1024 * 1024
BUNDLE_EXTS = {".ab", ".bundle", ".unity3d"}
CHARACTER = re.compile(r"char_\d+_[a-z0-9]+", re.I)
CATEGORIES = ("skinpack", "chararts", "charpack", "dynchars")


def safe_name(name):
    if not name or re.search(r'[<>:"/\\|?*\x00-\x1f]', name) or name.endswith((".", " ")) or name in (".", ".."):
        raise ValueError("Unsafe resource name")
    if re.match(r"^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)", name, re.I):
        raise ValueError("Reserved resource name")
    return name


def normalized(name):
    return re.sub(r"(?:\[alphe?\]|_alpha)(?=\.png$)", "[alpha]", name.lower().replace("#", "_").replace("@", "_")).removeprefix("illust_")


def source_files(root):
    files = []
    for item in root.rglob("*"):
        parts = item.relative_to(root).parts
        if any(p.startswith(".") or p.lower().startswith(("disable_", "disabled_")) or p.lower() in ("logs", "dump") for p in parts):
            continue
        if item.is_symlink() or not item.resolve().is_relative_to(root):
            raise ValueError("Linked source is not supported")
        if item.is_file() and not re.match(r"^(preview[.]|_cover|readme[.]|tags[.]json$|info[.]json$)", item.name, re.I):
            files.append(item)
        if len(files) > 20000:
            raise ValueError("Too many files")
    return sorted(files)


def bundle_records(file, return_env=False):
    if file.stat().st_size > MAX_BUNDLE:
        raise ValueError("Bundle exceeds 512 MiB")
    # Loading bytes keeps UnityPy dependency resolution within the supplied AB.
    env = UnityPy.load(file.read_bytes())
    records = []
    platforms = sorted({int(asset.target_platform) for asset in env.assets})
    skipped = []
    for asset in env.assets:
        objects = asset.objects
        trees = {}
        for oid, obj in objects.items():
            if obj.type.name == "MonoBehaviour":
                try:
                    trees[oid] = obj.read_typetree()
                except Exception:
                    pass

        def resolve(pointer):
            if not isinstance(pointer, dict) or pointer.get("m_FileID") != 0:
                return None
            return objects.get(pointer.get("m_PathID"))

        def add(obj, target, kind):
            target = safe_name(target)
            records.append({"target": target, "kind": kind, "object": obj, "source": file.name, "asset": obj.read().m_Name})

        # Static portraits use the prefab's dependency span to distinguish the
        # illustration texture from the same-named battle and base textures.
        for obj in objects.values():
            if obj.type.name != "AssetBundle":
                continue
            data = obj.read()
            for container, info in data.m_Container:
                stem = Path(container).stem
                prefix = ""
                if stem.startswith("illust_"):
                    prefix = stem
                elif "/charavatars/" in container or "/charportraits/" in container:
                    prefix = re.sub(r"^(dyn/|arts/)+", "", container).rsplit(".", 1)[0].replace("/", "_")
                if not prefix:
                    continue
                for ref in data.m_PreloadTable[info.preloadIndex:info.preloadIndex + info.preloadSize]:
                    texture = objects.get(ref.m_PathID) if ref.m_FileID == 0 else None
                    if not texture or texture.type.name != "Texture2D":
                        continue
                    name = texture.read().m_Name
                    bare = prefix.removeprefix("illust_")
                    alpha = re.search(r"(\[alpha\]|\[alphe\]|_alpha)$", name, re.I)
                    base_name = name[:alpha.start()] if alpha else name
                    if base_name not in (stem, bare) and not prefix.startswith(("charavatars_", "charportraits_")):
                        continue
                    add(texture, prefix + ("[alpha]" if alpha else "") + ".png", "texture")

        # Follow SkeletonAnimation -> SkeletonData -> atlas -> material -> texture.
        for tree in trees.values():
            skeleton = resolve(tree.get("skeletonJSON"))
            if not skeleton or skeleton.type.name != "TextAsset":
                continue
            skeleton_id = next((oid for oid, value in trees.items() if value is tree), None)
            base = re.sub(r"\.(skel|json)(?:\.txt)?$", "", skeleton.read().m_Name, flags=re.I)
            directions = set()
            for component in trees.values():
                if component.get("skeletonDataAsset", {}).get("m_PathID") == skeleton_id:
                    go = resolve(component.get("m_GameObject"))
                    if go and go.type.name == "GameObject":
                        direction = go.read().m_Name.lower()
                        if direction in ("front", "back"):
                            directions.add(direction)
            if len(directions) == 1:
                key = next(iter(directions)) + "_" + base
            elif not directions and base.startswith(("build_", "dyn_illust_", "entry_dyn_illust_")):
                key = base
            else:
                skipped.append({"code": "ambiguousSpine", "file": file.name, "asset": base})
                continue
            group = [(skeleton, key + (".json" if skeleton.read().m_Name.endswith(".json") else ".skel"), "spine")]
            atlas_refs = tree.get("atlasAssets", [])
            if len(atlas_refs) != 1:
                skipped.append({"code": "multiPage", "file": file.name, "asset": base})
                continue
            atlas_tree = trees.get(atlas_refs[0].get("m_PathID"), {})
            atlas = resolve(atlas_tree.get("atlasFile"))
            materials = atlas_tree.get("materials", [])
            if not atlas or len(materials) != 1:
                skipped.append({"code": "multiPage", "file": file.name, "asset": base})
                continue
            group.append((atlas, key + ".atlas", "spine"))
            material = resolve(materials[0])
            if not material:
                skipped.append({"code": "missingDependency", "file": file.name, "asset": base})
                continue
            props = material.read_typetree().get("m_SavedProperties", {}).get("m_TexEnvs", [])
            textures = dict(props)
            for prop, suffix in (("_MainTex", ""), ("_AlphaTex", "[alpha]")):
                texture = resolve(textures.get(prop, {}).get("m_Texture"))
                if texture and texture.type.name == "Texture2D":
                    group.append((texture, key + suffix + ".png", "texture"))
            if not any(item[2] == "texture" for item in group):
                skipped.append({"code": "missingDependency", "file": file.name, "asset": base})
                continue
            for item in group:
                add(*item)
    unique = {}
    ambiguous = set()
    for record in records:
        key = normalized(record["target"])
        if key in unique and unique[key]["object"] is not record["object"]:
            ambiguous.add(key)
        else:
            unique[key] = record
    for key in ambiguous:
        skipped.append({"code": "ambiguousAsset", "file": file.name, "asset": unique[key]["target"]})
        del unique[key]
    result = (list(unique.values()), skipped, platforms)
    return (*result, env) if return_env else result


def game_roots(game_path):
    if not game_path:
        return []
    exe = Path(game_path).resolve()
    if exe.name.lower() != "arknights.exe" or not exe.is_file():
        raise ValueError("Select Arknights.exe")
    data = exe.parent / "Arknights_Data"
    return [data / "PersistentData/Bundles", data / "StreamingAssets/AB/Windows"]


def current_records(roots, ids):
    found = []
    warnings = []
    digest = hashlib.sha256()
    for char_id in sorted(ids):
        for category in CATEGORIES:
            file = next((root / category / (char_id + ".ab") for root in roots if (root / category / (char_id + ".ab")).is_file()), None)
            if not file:
                continue
            try:
                digest.update((category + "/" + file.name).encode("utf-8"))
                with file.open("rb") as stream:
                    digest.update(hashlib.file_digest(stream, "sha256").digest())
                records, _, _ = bundle_records(file)
                found.extend(records)
            except Exception as error:
                warnings.append({"code": "baselineUnreadable", "file": category + "/" + file.name, "detail": str(error)[:240]})
    return found, warnings, digest.hexdigest()


def run(request):
    if request.get("action") == "build-bundles":
        from bundle_builder import build_bundles
        return build_bundles(request)
    root = Path(request["source"]).resolve(strict=True)
    roots = game_roots(request.get("gamePath"))
    output = Path(request["output"]).resolve() if request.get("output") else None
    if output:
        if output.exists() or output.is_relative_to(root) or root.is_relative_to(output):
            raise ValueError("Output must be a new, separate directory")
        if request.get("gamePath") and output.is_relative_to(Path(request["gamePath"]).resolve().parent):
            raise ValueError("Game directory is read-only")
    inputs = source_files(root)
    bundles = [file for file in inputs if file.suffix.lower() in BUNDLE_EXTS]
    ids = {match.lower() for file in inputs for match in CHARACTER.findall(file.name)}
    if len(ids) > 40 or len(bundles) > 100:
        raise ValueError("Process one character Mod at a time")
    baseline, warnings, baseline_fingerprint = current_records(roots, ids)
    reference = {}
    baseline_sizes = {}
    for record in baseline:
        reference.setdefault(normalized(record["target"]), set()).add(record["target"])
        if record["kind"] == "texture":
            obj = record["object"].read()
            baseline_sizes[record["target"]] = (obj.m_Width, obj.m_Height)
    candidates = []
    platforms = set()
    if bundles:
        for bundle in bundles:
            records, skipped, targets = bundle_records(bundle)
            warnings.extend(skipped)
            candidates.extend(records)
            platforms.update(targets)
        warnings.append({"code": "resourceOnly"})
        for file in inputs:
            if file.suffix.lower() not in BUNDLE_EXTS:
                warnings.append({"code": "bundleCompanion", "file": file.relative_to(root).as_posix()})
    else:
        for file in inputs:
            if file.suffix.lower() not in (".png", ".atlas", ".skel", ".json", ".wav", ".txt", ".ini"):
                warnings.append({"code": "unsupportedFile", "file": file.relative_to(root).as_posix()})
                continue
            candidates.append({"target": file.name, "kind": "loose", "file": file, "source": file.relative_to(root).as_posix(), "asset": file.name})
    plans = []
    payloads = {}
    for item in candidates:
        choices = reference.get(normalized(item["target"]), set())
        matched = len(choices) == 1
        name = next(iter(choices)) if matched else item["target"]
        # Only an exact, unique current resource identity may repair old naming.
        if len(choices) > 1:
            warnings.append({"code": "ambiguousAsset", "file": item["source"], "asset": name})
            continue
        if item["kind"] == "loose":
            relative = item["file"].relative_to(root).with_name(safe_name(name)).as_posix()
            data = item["file"].read_bytes()
            if item["file"].suffix.lower() == ".png":
                from PIL import Image
                with Image.open(io.BytesIO(data)) as image:
                    image.verify()
        else:
            relative = safe_name(name)
            obj = item["object"].read()
            if item["kind"] == "texture":
                image = obj.image.convert("RGBA")
                if matched and baseline_sizes.get(name) and image.size != baseline_sizes[name]:
                    warnings.append({"code": "dimensionsChanged", "file": name, "asset": f"{image.width}×{image.height} → {baseline_sizes[name][0]}×{baseline_sizes[name][1]}"})
                if image.width * image.height > 64 * 1024 * 1024:
                    raise ValueError("Texture is too large")
                stream = io.BytesIO()
                image.save(stream, format="PNG")
                data = stream.getvalue()
            else:
                script = obj.m_Script
                data = script.encode("utf-8", "surrogateescape") if isinstance(script, str) else bytes(script)
        key = relative.casefold()
        if key in payloads:
            if payloads[key][1] != data:
                raise ValueError("Conflicting output resource: " + relative)
            continue
        payloads[key] = (relative, data)
        plans.append({"source": item["source"], "asset": item["asset"], "target": relative, "matched": matched, "changed": name != item["target"], "bytes": len(data)})
    if not plans:
        raise ValueError("No supported resource bindings found")
    if not roots:
        warnings.append({"code": "noBaseline"})
    elif any(not plan["matched"] and CHARACTER.search(plan["target"]) for plan in plans):
        warnings.append({"code": "unmatched"})
    if output:
        output.mkdir(parents=True)
        for relative, data in payloads.values():
            target = output / relative
            if not target.resolve().is_relative_to(output):
                raise ValueError("Invalid output path")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    return {"success": True, "kind": "bundle" if bundles else "m4a", "platforms": sorted(platforms), "plans": plans, "warnings": warnings, "matched": sum(p["matched"] for p in plans), "renamed": sum(p["changed"] for p in plans), "baselineCount": len(baseline), "baselineFingerprint": baseline_fingerprint}


if __name__ == "__main__":
    try:
        request = json.loads(sys.stdin.buffer.read(1024 * 1024).decode("utf-8"))
        with contextlib.redirect_stdout(sys.stderr):
            result = run(request)
    except Exception as error:
        result = {"success": False, "error": str(error)[:1000]}
    sys.stdout.buffer.write(json.dumps(result, ensure_ascii=False).encode("utf-8"))
