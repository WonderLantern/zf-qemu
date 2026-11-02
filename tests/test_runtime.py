"""Portable launcher contracts using synthetic, temporary inputs only."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from zf_qemu import runtime
from zf_qemu.media import create_card
from zf_qemu.sd_upload import add_file


class RuntimeTests(unittest.TestCase):
    def test_local_images_are_readable_without_hash_or_size_pinning(self):
        self.assertTrue(runtime.LOCAL_IMAGES.isdisjoint(runtime.EXPECTED))
        with tempfile.TemporaryDirectory() as directory:
            for name in runtime.LOCAL_IMAGES:
                path = Path(directory) / name
                for payload in (b'a', b'a different local state image'):
                    path.write_bytes(payload)
                    with patch('zf_qemu.runtime.identity', side_effect=AssertionError('must not hash local images')):
                        result = runtime.check_input(name, path)
                    self.assertEqual(result['path'], str(path.resolve()))
                    self.assertNotIn('bytes', result)
                    self.assertNotIn('sha256', result)
                with self.assertRaisesRegex(ValueError, 'Missing'):
                    runtime.check_input(name, path.with_name('missing'))

    def test_hash_mismatch_is_rejected_even_at_the_expected_size(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sample'
            path.write_bytes(b'abc')
            expected = {'bytes': 3, 'sha256': hashlib.sha256(b'abc').hexdigest()}
            with patch.dict(runtime.EXPECTED, synthetic=expected):
                self.assertEqual(runtime.check_input('synthetic', path)['sha256'], expected['sha256'])
                path.write_bytes(b'xyz')
                with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                    runtime.check_input('synthetic', path)

    def test_config_resolves_relative_paths_without_workstation_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'settings.json'
            path.write_text(json.dumps({'qemu': 'build/qemu-system-aarch64',
                                        'inputs': {key: 'inputs/' + key for key in runtime.REQUIRED},
                                        'state_dir': 'state'}))
            config = runtime.Configuration.load(path)
            self.assertEqual(config.inputs['li'], root / 'inputs/li')
            self.assertEqual(config.qemu_img, root / 'build/qemu-img')
            self.assertEqual(config.state_dir, root / 'state')
            data = json.loads(path.read_text())
            data['inputs']['nand'] = 'inputs/nand'
            path.write_text(json.dumps(data))
            self.assertEqual(runtime.Configuration.load(path).inputs['nand'], root / 'inputs/nand')
            del data['inputs']['li']
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'missing=.*li'):
                runtime.Configuration.load(path)

    def test_shared_command_contains_li_and_keeps_media_immutable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = {key: root / key for key in runtime.REQUIRED}
            config = runtime.Configuration(inputs, Path('/bin/true'), Path('/bin/true'), root, {})
            run = config.new_run()
            with patch('zf_qemu.runtime.subprocess.run') as call:
                command = config.command(run, {}, root / 'sensor', [])
            self.assertEqual(call.call_count, 2)
            machine = command[command.index('-machine') + 1]
            self.assertIn('li-image=' + str(inputs['li']), machine)
            self.assertIn('mcu-image=' + str(inputs['mcu']), machine)
            self.assertIn('zf-sd-drive=zf-sd', machine)
            self.assertIn('zf-microsd-drive=zf-microsd', machine)
            self.assertIn('-nic', command)
            self.assertNotIn('-plugin', command)
            self.assertNotIn('-netdev', command)
            with self.assertRaisesRegex(ValueError, 'validated inputs'):
                config.command(run, {}, root / 'sensor', ['li-image=other'])

    def test_fresh_cards_accept_uploads_and_preserve_both_fats(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'card'
            create_card(path)
            result = add_file(path, 'TEST.TXT', b'synthetic upload')
            self.assertTrue(result)
            with self.assertRaisesRegex(ValueError, '已存在'):
                add_file(path, 'TEST.TXT', b'duplicate')
            with path.open('rb') as stream:
                stream.seek(2048 * 512)
                boot = stream.read(512)
                reserved, fat_sectors = struct.unpack_from('<H', boot, 14)[0], struct.unpack_from('<I', boot, 36)[0]
                stream.seek((2048 + reserved) * 512)
                first = stream.read(fat_sectors * 512)
                self.assertEqual(first, stream.read(fat_sectors * 512))
            with self.assertRaises(ValueError):
                add_file(path, '../other', b'bad name')

    def test_vm_lock_survives_parent_descriptor_close(self):
        with tempfile.TemporaryDirectory() as directory:
            config = runtime.Configuration({}, Path('/bin/true'), Path('/bin/true'), Path(directory), {})
            with config.idle_guard() as lock:
                child = subprocess.Popen(['sleep', '1'], pass_fds=(lock.fileno(),))
            try:
                with self.assertRaisesRegex(RuntimeError, 'already using'):
                    with config.idle_guard():
                        pass
            finally:
                child.terminate()
                child.wait()
            with config.idle_guard():
                pass


if __name__ == '__main__':
    unittest.main()
