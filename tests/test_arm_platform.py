"""Validate matching ACPI and compiled device-tree CPU topology."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_arm_platform import build


@unittest.skipUnless(shutil.which('dtc'), 'device-tree compiler is required')
class ArmPlatform(unittest.TestCase):
    def test_matching_descriptions(self):
        for count in (2, 8):
            with self.subTest(cpus=count), tempfile.TemporaryDirectory() as directory:
                output = build(count, Path(directory))
                tree = subprocess.check_output(['dtc', '-I', 'dtb', '-O', 'dts',
                                                str(output / 'pcemu-virt.dtb')], text=True)
                self.assertEqual(tree.count('device_type = "cpu";'), count)
                for cpu in range(count):
                    self.assertIn(f'cpu@{cpu:x} {{', tree)
                self.assertIn(f'0x80a0000 0x00 0x{count * 0x20000:x}', tree)
                manifest = json.loads((output / 'acpi/manifest.json').read_text())
                self.assertEqual(manifest['cpuCount'], count)
                self.assertIn('PPTT', manifest['tableOffsets'])

    def test_invalid_count(self):
        for count in (0, 9):
            with self.assertRaises(ValueError):
                build(count, Path('/unused'))


if __name__ == '__main__':
    unittest.main()
