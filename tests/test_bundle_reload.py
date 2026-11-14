"""A scoped disable must not parse or rebuild unrelated Unity packages."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from bundle_builder import build_bundles, digest, input_fingerprint
from pc_layout import _bundle_index


class ScopedReload(unittest.TestCase):
    def test_disabled_mod_removes_only_its_bundle(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game = root / 'game' / 'Arknights.exe'
            game.parent.mkdir()
            game.write_bytes(b'never executed')
            bundles = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack'
            bundles.mkdir(parents=True)
            for name in ('a.ab', 'b.ab'):
                (bundles / name).write_bytes(b'original')
            source, scope = root / 'enabled', root / 'disabled'
            for folder, name, bundle in ((source, 'b.png', 'skinpack/b.ab'), (scope, 'a.png', 'skinpack/a.ab')):
                folder.mkdir()
                (folder / name).write_bytes(b'pixels')
                (folder / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [{'file': name, 'target': name, 'bundle': bundle}]}))
            read = []
            def records(file, return_env=False):
                read.append(file.name)
                self.assertEqual(file.name, 'a.ab', 'An unrelated bundle was parsed')
                result = ([{'target': 'a.png'}], [], [19])
                return (*result, None) if return_env else result
            try:
                with patch('bundle_builder.bundle_records', records):
                    output = root / 'patch'
                    result = build_bundles({'source': str(source), 'scopeSource': str(scope), 'gamePath': str(game), 'output': str(output), 'affectedBundles': ['skinpack/a.ab']})
                self.assertEqual(result['bundles'], [])
                self.assertEqual([item['logical'] for item in result['removedBundles']], ['skinpack/a.ab'])
                self.assertTrue((output / 'manifest.tsv').read_text().startswith('QAQR-ARKNIGHTS-PATCH\t1\n'))
                self.assertNotIn('b.ab', (output / 'manifest.tsv').read_text())
                self.assertEqual((bundles / 'b.ab').read_bytes(), b'original')
                self.assertEqual(read, [], 'Bound removals should not parse Unity packages')
            finally:
                _bundle_index.cache_clear()


    def test_one_patch_covers_several_disabled_mods(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game = root / 'game' / 'Arknights.exe'
            game.parent.mkdir()
            game.write_bytes(b'never executed')
            bundles = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack'
            bundles.mkdir(parents=True)
            for name in ('a.ab', 'b.ab', 'c.ab'):
                (bundles / name).write_bytes(b'original')
            source, first, second = root / 'enabled', root / 'first', root / 'second'
            for folder, name, bundle in ((source, 'b.png', 'skinpack/b.ab'), (first, 'a.png', 'skinpack/a.ab'), (second, 'c.png', 'skinpack/c.ab')):
                folder.mkdir()
                (folder / name).write_bytes(b'pixels')
                (folder / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [{'file': name, 'target': name, 'bundle': bundle}]}))
            read = []
            def records(file, return_env=False):
                read.append(file.name)
                self.assertIn(file.name, ('a.ab', 'c.ab'), 'An unrelated bundle was parsed')
                result = ([{'target': file.stem + '.png'}], [], [19])
                return (*result, None) if return_env else result
            try:
                with patch('bundle_builder.bundle_records', records):
                    output = root / 'patch'
                    result = build_bundles({'source': str(source), 'scopeSources': [str(first), str(second)], 'gamePath': str(game), 'output': str(output), 'affectedBundles': ['skinpack/a.ab', 'skinpack/c.ab']})
                self.assertEqual(result['bundles'], [])
                self.assertEqual(sorted(item['logical'] for item in result['removedBundles']), ['skinpack/a.ab', 'skinpack/c.ab'])
                manifest = (output / 'manifest.tsv').read_text()
                self.assertTrue(manifest.startswith('QAQR-ARKNIGHTS-PATCH\t1\n'))
                self.assertNotIn('b.ab', manifest)
                self.assertEqual(read, [], 'Bound removals should not parse Unity packages')
                with self.assertRaises(ValueError):
                    build_bundles({'source': str(source), 'scopeSources': [], 'gamePath': str(game), 'output': str(root / 'empty')})
            finally:
                _bundle_index.cache_clear()

    def test_reuses_verified_bundle_without_parsing_and_rebuilds_changed_content(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game = root / 'game/Arknights.exe'
            game.parent.mkdir()
            game.write_bytes(b'never executed')
            original = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack/a.ab'
            original.parent.mkdir(parents=True)
            original.write_bytes(b'original')
            source, cache = root / 'resources', root / 'cache'
            source.mkdir()
            resource = source / 'a.png'
            resource.write_bytes(b'pixels')
            (source / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [
                {'file': 'a.png', 'target': 'a.png', 'bundle': 'skinpack/a.ab'}]}))
            replacement = cache / 'bundles/skinpack/a.ab'
            replacement.parent.mkdir(parents=True)
            replacement.write_bytes(b'previously generated fixture')
            entry = {'logical': 'skinpack/a.ab', 'source': original.relative_to(game.parent).as_posix(),
                'replacement': 'bundles/skinpack/a.ab', 'sourceSha256': digest(original), 'replacementSha256': digest(replacement),
                'inputs': ['a.png'], 'resources': ['a.png'],
                'inputFingerprint': input_fingerprint([{'file': resource, 'key': 'a.png'}], source, digest, set())}
            (cache / 'bundle-index.json').write_text(json.dumps({'schemaVersion': 1, 'bundles': [entry]}))
            request = {'source': str(source), 'gamePath': str(game), 'cacheDirectory': str(cache)}
            with patch('bundle_builder.bundle_records', side_effect=AssertionError('rebuild required')):
                result = build_bundles({**request, 'output': str(root / 'reused')})
                self.assertEqual(result['reusedBundleCount'], 1)
                self.assertEqual((root / 'reused/bundles/skinpack/a.ab').read_bytes(), replacement.read_bytes())
                for file, label in ((resource, 'input'), (original, 'game'), (replacement, 'cache')):
                    before = file.read_bytes()
                    file.write_bytes(b'changed')
                    with self.assertRaisesRegex(AssertionError, 'rebuild required'):
                        build_bundles({**request, 'output': str(root / ('rebuild-' + label))})
                    file.write_bytes(before)

    def test_batch_disable_handles_five_hundred_bundles_without_parsing(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game = root / 'game/Arknights.exe'
            game.parent.mkdir()
            game.write_bytes(b'never executed')
            bundles = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack'
            bundles.mkdir(parents=True)
            source, scope = root / 'enabled', root / 'disabled'
            source.mkdir(); scope.mkdir()
            resources = []
            for i in range(500):
                (bundles / f'{i}.ab').write_bytes(b'original')
                (scope / f'{i}.png').write_bytes(b'pixels')
                resources.append({'file': f'{i}.png', 'target': f'{i}.png', 'bundle': f'skinpack/{i}.ab'})
            (scope / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': resources}))
            with patch('bundle_builder.bundle_records', side_effect=AssertionError('Unnecessary Unity parsing')):
                result = build_bundles({'source': str(source), 'scopeSource': str(scope), 'gamePath': str(game), 'output': str(root / 'output')})
            self.assertEqual(len(result['removedBundles']), 500)


if __name__ == '__main__':
    unittest.main()
