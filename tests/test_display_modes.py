import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('stage_display_driver',
    Path(__file__).resolve().parents[1] / 'tools/stage_display_driver.py')
# The staging module imports its adjacent FAT utilities.
import sys
sys.path.insert(0, str(Path(spec.origin).parent))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

class DisplayModesTest(unittest.TestCase):
    def test_custom_mode_only_changes_driver_registry_section(self):
        original = b'[version]\r\nClass=DISPLAY\r\n[VM.AddReg]\r\nHKR,DEFAULT,vdd,,"*vdd"\r\n\r\n[Other]\r\nKeep=unchanged\r\n'
        patched = stage.add_widescreen_mode(original)
        for bpp in (8,16,24,32):
            row = ('HKR,"MODES\\%d\\1440,900"\r\n' % bpp).encode()
            self.assertEqual(patched.count(row), 1)
            patched = patched.replace(row,b'')
        self.assertEqual(patched, original)
        result = stage.add_widescreen_mode(original)
        self.assertEqual(stage.add_widescreen_mode(result), result)

    def test_mode_in_another_section_does_not_hide_missing_mode(self):
        inf = b'[VM.AddReg]\r\nHKR,DEFAULT,vdd,,"*vdd"\r\n\r\n[Other]\r\nHKR,"MODES\\32\\1440,900"\r\n'
        patched = stage.add_widescreen_mode(inf)
        self.assertEqual(patched.count(b'"MODES\\32\\1440,900"'), 2)
        self.assertEqual(stage.add_widescreen_mode(patched), patched)

    def test_refuses_unexpected_driver_layout(self):
        for content in (b'[Other]\r\n', b'[VM.AddReg]\r\n',
                        b'[VM.AddReg]\r\n[VM.AddReg]\r\n[Other]\r\n'):
            with self.assertRaises(ValueError):
                stage.add_widescreen_mode(content)

if __name__ == '__main__':
    unittest.main()
