"""User-facing dial order and persistent synthetic lens input contracts."""
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from zf_qemu import gui_server, lens, retro_dial_positions as retro


class ControlTests(unittest.TestCase):
    def test_shutter_order_keeps_existing_electrical_assignments(self):
        self.assertEqual(retro.labels(0), [
            '1/3 STEP', '1/8000 s', '1/4000 s', '1/2000 s', '1/1000 s',
            '1/500 s', '1/250 s', '1/125 s', '1/60 s', '1/30 s', '1/15 s',
            '1/8 s', '1/4 s', '1/2 s', '1 s', '2 s', '4 s', 'B', 'T', 'X'])
        self.assertEqual([retro.describe(0, p)['code'] for p in range(20)],
                         [0, 19, *range(4, 19), 3, 2, 1])
        self.assertEqual(retro.mask_for(0, 0), retro.DEFAULT_MASK[0])
        self.assertEqual(retro.mask_for(0, 1), 55)
        self.assertEqual([retro.mask_for(0, p) for p in (17, 18, 19)], [50, 51, 49])

    def test_lens_validation_and_restart_persistence(self):
        with tempfile.TemporaryDirectory() as directory:
            config = types.SimpleNamespace(state_dir=Path(directory))
            with patch.object(gui_server, 'CONFIG', config):
                session = gui_server.Session()
                self.assertEqual(session.lens_settings, lens.DEFAULT)
                settings = {'focal_length_mm': 1200, 'aperture_x100': 1600}
                self.assertTrue(session.control({'kind': 'lens', **settings})['accepted'])
                self.assertEqual(gui_server.Session().lens_settings, settings)
                self.assertEqual(session.snapshot()['lens_settings'], settings)
                for invalid in ({**settings, 'focal_length_mm': 5},
                                {**settings, 'focal_length_mm': 1201},
                                {**settings, 'focal_length_mm': 50.5},
                                {**settings, 'focal_length_mm': True},
                                {**settings, 'aperture_x100': 110}):
                    with self.assertRaises(ValueError):
                        session.control({'kind': 'lens', **invalid})
                self.assertEqual(gui_server.Session().lens_settings, settings)
                session.lens_path.write_text('invalid json')
                self.assertEqual(gui_server.Session().lens_settings, lens.DEFAULT)

    def test_runtime_lens_write_requires_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(gui_server, 'CONFIG', types.SimpleNamespace(state_dir=Path(directory))):
                session = gui_server.Session()
            session.proc = Mock()
            session.proc.poll.return_value = None
            session.qmp = Mock(side_effect=[None, '6:120'])
            settings = {'focal_length_mm': 6, 'aperture_x100': 120}
            session.control({'kind': 'lens', **settings})
            self.assertEqual(session.qmp.call_args_list[0].args,
                             ('qom-set', {'path': '/machine', 'property': 'zf-lens-settings', 'value': '6:120'}))
            self.assertEqual(json.loads(session.lens_path.read_text()), settings)
            session.qmp = Mock(side_effect=[None, '50:100'])
            with self.assertRaisesRegex(RuntimeError, '读回'):
                session.control({'kind': 'lens', 'focal_length_mm': 50, 'aperture_x100': 1000})
            self.assertEqual(json.loads(session.lens_path.read_text()), settings)


if __name__ == '__main__':
    unittest.main()
