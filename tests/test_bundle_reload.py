"""A scoped disable must not parse or rebuild unrelated Unity packages."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from bundle_builder import build_bundles, digest, input_fingerprint, input_fingerprints
from pc_layout import _bundle_index
from worker import normalized


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

    def cached_library(self, root, names):
        """A game, a bound resource per name, and a finished build that already holds each bundle."""
        game = root / 'game/Arknights.exe'
        game.parent.mkdir()
        game.write_bytes(b'never executed')
        originals = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack'
        originals.mkdir(parents=True)
        source, cache = root / 'resources', root / 'cache'
        source.mkdir()
        entries = {}
        for name in names:
            (originals / (name + '.ab')).write_bytes(b'original ' + name.encode())
            (source / (name + '.png')).write_bytes(b'pixels ' + name.encode())
            replacement = cache / 'bundles/skinpack' / (name + '.ab')
            replacement.parent.mkdir(parents=True, exist_ok=True)
            replacement.write_bytes(b'generated ' + name.encode())
            entries[name] = {'logical': f'skinpack/{name}.ab', 'source': f'Arknights_Data/StreamingAssets/AB/Windows/skinpack/{name}.ab',
                'replacement': f'bundles/skinpack/{name}.ab', 'sourceSha256': digest(originals / (name + '.ab')), 'replacementSha256': digest(replacement),
                'inputs': [name + '.png'], 'resources': [name + '.png']}
        (source / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [
            {'file': name + '.png', 'target': name + '.png', 'bundle': f'skinpack/{name}.ab'} for name in names]}))
        return game, source, cache, entries

    def test_conflict_approval_rebuilds_only_the_bundle_it_concerns(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game, source, cache, entries = self.cached_library(root, ('a', 'b'))
            item = lambda name: [{'file': source / (name + '.png'), 'key': name + '.png'}]
            unrelated = {(('x.png', '0' * 64), ('y.png', '1' * 64))}
            half = (('a.png', digest(source / 'a.png')), ('z.png', 'f' * 64))
            # a was built before approvals existed; b by a build that counted every approval for every bundle.
            entries['a']['inputFingerprint'] = input_fingerprints(item('a'), source, digest, set())[0]
            entries['b']['inputFingerprint'] = input_fingerprints(item('b'), source, digest, unrelated)[1]
            self.assertNotEqual(entries['b']['inputFingerprint'], input_fingerprints(item('b'), source, digest, unrelated)[0])
            (cache / 'bundle-index.json').write_text(json.dumps({'schemaVersion': 1, 'bundles': list(entries.values())}))
            marker = lambda pairs: (source / '.qaqr-m4a.json').write_text(json.dumps({'owner': 'qaqr-arknights-v1', 'modOrder': [],
                'approvedResourcePairs': [[{'file': file, 'sha256': sha} for file, sha in files] for files in pairs]}))
            request = {'source': str(source), 'gamePath': str(game)}
            try:
                with patch('bundle_builder.bundle_records', side_effect=AssertionError('rebuild required')):
                    marker(unrelated)
                    result = build_bundles({**request, 'cacheDirectory': str(cache), 'output': str(root / 'migrated')})
                    self.assertEqual(result['reusedBundleCount'], 2)
                    stored = {entry['logical']: entry['inputFingerprint'] for entry in result['bundles']}
                    self.assertEqual(stored['skinpack/b.ab'], input_fingerprints(item('b'), source, digest, unrelated)[0], 'A reused bundle takes the current fingerprint')
                    (root / 'migrated/bundle-index.json').write_text(json.dumps({'schemaVersion': 1, 'bundles': result['bundles']}))
                    # Another approval, with only one of its files among a bundle's inputs, changes nothing.
                    marker(unrelated | {half})
                    self.assertEqual(build_bundles({**request, 'cacheDirectory': str(root / 'migrated'), 'output': str(root / 'half')})['reusedBundleCount'], 2)
                (source / 'other.png').write_bytes(b'second edit of a')
                both = [*item('a'), {'file': source / 'other.png', 'key': 'a.png'}]
                concerned = {(('a.png', digest(source / 'a.png')), ('other.png', digest(source / 'other.png')))}
                self.assertNotEqual(input_fingerprints(both, source, digest, concerned)[0], input_fingerprints(both, source, digest, set())[0])
                self.assertEqual(input_fingerprints(item('b'), source, digest, concerned)[0], input_fingerprints(item('b'), source, digest, set())[0])
            finally:
                _bundle_index.cache_clear()

    def test_resource_without_binding_keeps_unchanged_bundles(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game, source, cache, entries = self.cached_library(root, ('a', 'char_002_amiya'))
            # The second resource loses its binding, as a loose Mod without a manifest has none.
            (source / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [{'file': 'a.png', 'target': 'a.png', 'bundle': 'skinpack/a.ab'}]}))
            for name in ('a', 'char_002_amiya'):
                entries[name]['inputFingerprint'] = input_fingerprint([{'file': source / (name + '.png'), 'key': normalized(name + '.png')}], source, digest, set())
            (cache / 'bundle-index.json').write_text(json.dumps({'schemaVersion': 1, 'bundles': list(entries.values())}))
            read = []
            def records(file, return_env=False):
                if return_env:
                    raise AssertionError('rebuild required: ' + file.name)
                read.append(file.name)
                return ([{'target': file.stem + '.png'}], [], [19])
            request = {'source': str(source), 'gamePath': str(game), 'cacheDirectory': str(cache)}
            try:
                with patch('bundle_builder.bundle_records', records):
                    result = build_bundles({**request, 'output': str(root / 'reused')})
                    self.assertEqual(result['reusedBundleCount'], 2)
                    self.assertEqual(sorted(read), ['a.ab', 'char_002_amiya.ab'], 'Unbound resources are matched by reading the bundles')
                    (source / 'char_002_amiya.png').write_bytes(b'edited')
                    with self.assertRaisesRegex(AssertionError, 'rebuild required: char_002_amiya.ab'):
                        build_bundles({**request, 'output': str(root / 'edited')})
            finally:
                _bundle_index.cache_clear()

    def test_interrupted_build_keeps_its_bundles_for_the_next_attempt(self):
        # One text object per package stands in for Unity serialization.
        class FakeData:
            def __init__(self, store):
                self.store, self.m_Script = store, store['script']
            def save(self):
                self.store['script'] = self.m_Script
        class FakeObject:
            def __init__(self, store):
                self.store, self.path_id = store, 1
                self.type, self.assets_file = SimpleNamespace(name='TextAsset'), SimpleNamespace(name='assets')
            def read(self):
                return FakeData(self.store)
            def get_raw_data(self):
                return self.store['script'].encode()
        class FakeEnv:
            def __init__(self, store):
                self.store, self.objects, self.assets = store, [FakeObject(store)], [SimpleNamespace(target_platform=19)]
                self.files = {'root': self}
            def save(self, packer):
                return json.dumps(self.store).encode()
        with tempfile.TemporaryDirectory(dir=os.environ.get('QAQR_TEST_TMP')) as directory:
            root = Path(directory)
            game = root / 'game/Arknights.exe'
            game.parent.mkdir()
            game.write_bytes(b'never executed')
            originals = game.parent / 'Arknights_Data/StreamingAssets/AB/Windows/skinpack'
            originals.mkdir(parents=True)
            source, work = root / 'resources', root / 'work'
            source.mkdir(); work.mkdir()
            for name in ('a', 'b'):
                (originals / (name + '.ab')).write_bytes(b'original ' + name.encode())
                (source / (name + '.skel')).write_bytes(b'skeleton ' + name.encode())
            (source / '.qaqr-resources.json').write_text(json.dumps({'schemaVersion': 1, 'resources': [
                {'file': name + '.skel', 'target': name + '.skel', 'bundle': f'skinpack/{name}.ab'} for name in ('a', 'b')]}))
            broken, rebuilt = {'b.ab'}, []
            def records(file, return_env=False):
                env = FakeEnv({'script': ''})
                result = ([{'target': file.stem + '.skel', 'kind': 'spine', 'object': env.objects[0]}], [], [13 if file.name in broken else 19])
                if return_env:
                    rebuilt.append(file.name)
                return (*result, env) if return_env else result
            request = {'source': str(source), 'gamePath': str(game)}
            leftovers = lambda: sorted(work.glob('.qaqr-bundles-*'))
            try:
                with patch('bundle_builder.bundle_records', records), patch('bundle_builder.UnityPy.load', lambda data: FakeEnv(json.loads(data))):
                    with self.assertRaisesRegex(ValueError, 'non-Windows'):
                        build_bundles({**request, 'output': str(work / 'plain')})
                    self.assertEqual(leftovers(), [], 'A build that cannot be resumed leaves nothing behind')
                    with self.assertRaisesRegex(ValueError, 'non-Windows'):
                        build_bundles({**request, 'output': str(work / 'first'), 'resumeDirectories': []})
                    self.assertEqual(len(leftovers()), 1)
                    partial = leftovers()[0] / 'bundle-index.partial.jsonl'
                    self.assertEqual([json.loads(line)['logical'] for line in partial.read_text().splitlines()], ['skinpack/a.ab'])
                    with partial.open('a') as stream:
                        stream.write('{"logical": "skinpack/b.ab", "replace')  # The line a killed build was writing.
                    broken.clear(); rebuilt.clear()
                    result = build_bundles({**request, 'output': str(work / 'second'), 'resumeDirectories': [str(leftovers()[0])]})
                    self.assertEqual((result['reusedBundleCount'], result['bundleCount'], rebuilt), (1, 2, ['b.ab']))
                    self.assertEqual(len(leftovers()), 1, 'The finished build moved to its output')
                    self.assertEqual(len((work / 'second/bundle-index.partial.jsonl').read_text().splitlines()), 2)
                    self.assertEqual(digest(work / 'second/bundles/skinpack/a.ab'), result['bundles'][0]['replacementSha256'])
                    # A finished build that never got its index is as good as a cache.
                    rebuilt.clear()
                    again = build_bundles({**request, 'output': str(work / 'third'), 'resumeDirectories': [str(work / 'second')]})
                    self.assertEqual((again['reusedBundleCount'], rebuilt), (2, []))
            finally:
                _bundle_index.cache_clear()

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
