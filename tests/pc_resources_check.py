"""Offline PC integration checks. Reads a user-selected game; never launches it."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from worker import bundle_records, run
from bundle_builder import build_bundles, digest
from pc_layout import RESOURCE_MANIFEST, current_bundles, game_roots, read_bindings


def verify(game, output, audit_dynamic=True):
    roots = game_roots(str(game))
    output = output.resolve()
    if output.is_relative_to(game.parent):
        raise ValueError('Test output must be outside the game')
    output.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='pc-layout-', dir=output))
    originals = dict(current_bundles(roots, {'char_2025_shu', 'char_1035_wisdel', 'char_003_kalts', 'char_124_kroos', 'char_2014_nian'}))
    before = {name: digest(file) for name, file in originals.items()}
    assert 'PersistentData' in originals['skinpack/char_2025_shu.ab'].parts
    assert 'arts/dynchars/char_2025_shu_nlc#1.ab' in originals
    assert 'arts/dynchars/char_2025_shu_2.ab' in originals
    changes = [
        ('spring', 'skinpack/char_2025_shu.ab', 'illust_char_2025_shu_nian#11.png'),
        ('spring', 'arts/dynchars/char_2025_shu_nian#11.ab', 'dyn_illust_char_2025_shu_nian#11.png'),
        ('spring', 'arts/dynchars/char_2025_shu_nian#11.ab', 'dyn_portrait_char_2025_shu_nian#11.png'),
        ('river', 'skinpack/char_2025_shu.ab', 'illust_char_2025_shu_nlc#1.png'),
        ('river', 'arts/dynchars/char_2025_shu_nlc#1.ab', 'dyn_illust_char_2025_shu_nlc#1.png'),
        ('default', 'arts/dynchars/char_2025_shu_2.ab', 'dyn_illust_char_2025_shu.png'),
        ('entrance', 'arts/dynchars/char_1035_wisdel_sale#14.ab', 'dyn_illust_char_1035_wisdel_sale#14_Start.png'),
        ('same-name-main', 'arts/dynchars/char_003_kalts_boc#6.ab', 'dyn_illust_char_003_kalts_boc#6.png'),
        ('same-name-entrance', 'arts/dynchars/char_003_kalts_boc#6.ab', 'dyn_entrance_char_003_kalts_boc#6.png'),
        ('special', 'arts/dynchars/char_124_kroos_sale#14.ab', 'sp_dyn_illust_char_124_kroos_sale#14.png'),
        ('multi-page', 'arts/dynchars/char_2014_nian_2.ab', 'dyn_illust_char_2014_nian2__page1.png'),
        ('multi-page', 'arts/dynchars/char_2014_nian_2.ab', 'dyn_illust_char_2014_nian2__page2.png'),
    ]
    manifests = collections.defaultdict(list)
    cache = {}
    for mod, bundle, target in changes:
        if bundle not in cache:
            cache[bundle] = bundle_records(originals[bundle])[0]
        record = next(record for record in cache[bundle] if record['target'].lower() == target.lower())
        image = record['object'].read().image.convert('RGBA')
        pixel = image.getpixel((0, 0))
        image.putpixel((0, 0), ((pixel[0] + 1) % 256, *pixel[1:]))
        folder = scratch / 'enabled' / mod
        folder.mkdir(parents=True, exist_ok=True)
        image.save(folder / target)
        manifests[folder].append({'file': target, 'target': target, 'bundle': bundle})
    for folder, resources in manifests.items():
        (folder / RESOURCE_MANIFEST).write_text(json.dumps({'schemaVersion': 1, 'resources': resources}), encoding='utf-8')
    result = build_bundles({'source': str(scratch / 'enabled'), 'gamePath': str(game), 'output': str(scratch / 'rebuilt')})
    assert result['bundleCount'] == 8, result
    assert result['resourceCount'] == 12, result
    skin = next(item for item in result['bundles'] if item['replacement'] == 'bundles/skinpack/char_2025_shu.ab')
    assert len(skin['resources']) == 2, skin

    # Feed the merged whole bundle back through conversion and select one skin.
    whole = scratch / 'whole-bundle' / 'skinpack'
    whole.mkdir(parents=True)
    shutil.copyfile(scratch / 'rebuilt' / skin['replacement'], whole / 'char_2025_shu.ab')
    request = {'source': str(whole.parent), 'gamePath': str(game)}
    analysis = run(request)
    assert sum(plan['comparison'] == 'different' for plan in analysis['plans']) == 2
    selected = [plan['id'] for plan in analysis['plans'] if plan['resource'] == 'illust_char_2025_shu_nian#11.png']
    assert len(selected) == 1
    converted = run({**request, 'output': str(scratch / 'selected'), 'selectedIds': selected})
    assert converted['written'] == 1
    assert len(read_bindings(scratch / 'selected')) == 1
    result_selected = build_bundles({'source': str(scratch / 'selected'), 'gamePath': str(game), 'output': str(scratch / 'selected-rebuilt')})
    assert result_selected['bundleCount'] == 1 and result_selected['resourceCount'] == 1
    # Streaming failure must leave no output or temporary package tree.
    broken = scratch / 'broken-resources'
    shutil.copytree(scratch / 'enabled/spring', broken)
    (broken / 'illust_char_2025_shu_nian#11.png').write_bytes(b'invalid PNG')
    failed_output = scratch / 'failed-bundles'
    try:
        build_bundles({'source': str(broken), 'gamePath': str(game), 'output': str(failed_output)})
        raise AssertionError('Corrupt image was accepted')
    except (ValueError, OSError):
        pass
    assert not failed_output.exists() and not list(scratch.glob('.qaqr-bundles-*'))

    loose = scratch / 'legacy-multipage'
    loose.mkdir()
    shutil.copyfile(scratch / 'enabled/multi-page/dyn_illust_char_2014_nian2__page2.png', loose / 'dyn_illust_char_2014_nian2__page2.png')
    hints = {'source': str(loose), 'gamePath': str(game), 'characterIds': ['char_2014_nian']}
    assert run(hints)['matched'] == 1
    assert build_bundles({**hints, 'output': str(scratch / 'loose-rebuilt')})['bundleCount'] == 1

    # Every installed/downloaded dynamic bundle is parsed, without decoding or
    # exporting full game assets. Record unsupported graph shapes for review.
    dynamic = {}
    for root in reversed(roots) if audit_dynamic else []:
        for file in (root / 'arts/dynchars').glob('*.ab'):
            dynamic[file.name.lower()] = file
    audit = []
    for name, file in sorted(dynamic.items()):
        checksum = digest(file)
        records, skipped, platforms = bundle_records(file)
        assert digest(file) == checksum
        assert all(platform in (5, 19) for platform in platforms)
        audit.append({'bundle': name, 'resources': len(records), 'multiPageResources': sum('__page' in record['target'] or '__atlas' in record['target'] for record in records), 'skipped': skipped})
        if len(audit) % 25 == 0:
            print(json.dumps({'dynamicBundlesRead': len(audit)}), flush=True)
    assert all(digest(originals[name]) == expected for name, expected in before.items())
    report = {'success': True, 'gameStarted': False, 'originalFilesUnchanged': True, 'bundleCount': result['bundleCount'], 'resourceCount': result['resourceCount'], 'selectedCopyCount': converted['written'], 'dynamicBundlesRead': len(audit), 'dynamicAudit': audit}
    (scratch / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({**{key: value for key, value in report.items() if key != 'dynamicAudit'}, 'report': str(scratch / 'report.json')}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--skip-dynamic-audit', action='store_true')
    args = parser.parse_args()
    verify(args.game.resolve(), args.output, not args.skip_dynamic_audit)
