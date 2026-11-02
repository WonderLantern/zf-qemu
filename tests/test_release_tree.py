"""Negative controls for the public release scanner."""
import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('release_scan', Path(__file__).with_name('check_release_tree.py'))
scanner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scanner)


class ReleaseTests(unittest.TestCase):
    def test_dump_private_path_and_key_are_rejected(self):
        for name, text in [('qemu/capture.bin', 'dump'),
                           ('docs/leak.md', '/home/private-user/experiment/file'),
                           ('docs/leak.md', '-----BEGIN PRIVATE KEY-----')]:
            with self.subTest(name=name, text=text), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / name
                path.parent.mkdir(parents=True)
                path.write_text(text)
                self.assertTrue(scanner.scan(root))

    def test_synthetic_source_and_hash_list_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'README.md').write_text('Use local, user-supplied inputs.')
            (root / 'docs').mkdir()
            (root / 'docs/inputs.md').write_text('SHA256: ' + 'a' * 64)
            self.assertEqual(scanner.scan(root), [])

    def test_only_the_reviewed_documentation_image_is_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / 'docs/images/webui.png'
            image.parent.mkdir(parents=True)
            image.write_bytes(b'synthetic image fixture')
            digest = hashlib.sha256(image.read_bytes()).hexdigest()
            with patch.dict(scanner.PUBLIC_IMAGES, {'docs/images/webui.png': digest}, clear=True):
                self.assertEqual(scanner.scan(root), [])
                image.write_bytes(b'changed image')
                self.assertTrue(scanner.scan(root))
                image.rename(image.with_name('other.png'))
                self.assertTrue(scanner.scan(root))


if __name__ == '__main__':
    unittest.main()
