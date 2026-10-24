"""A scoped disable must not parse or rebuild unrelated Unity packages."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from bundle_builder import build_bundles
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
                self.assertEqual(read, ['a.ab'])
            finally:
                _bundle_index.cache_clear()


if __name__ == '__main__':
    unittest.main()
