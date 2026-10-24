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

    def test_eight_gib_memory_description(self):
        with tempfile.TemporaryDirectory() as directory:
            output = build(1, Path(directory), 8192)
            memory = subprocess.check_output(['fdtget', '-t', 'x', str(output / 'pcemu-virt.dtb'),
                                              '/memory@40000000', 'reg'], text=True)
            self.assertEqual(memory.split(), ['0', '40000000', '2', '0'])

    def test_hyperv_identity_is_opt_in(self):
        for enabled in (False, True):
            with tempfile.TemporaryDirectory() as directory:
                output = build(1, Path(directory), 8192, enabled)
                manifest = json.loads((output / 'acpi/manifest.json').read_text())
                tables = (output / 'acpi/tables').read_bytes()
                offset = manifest['tableOffsets']['FACP']
                self.assertEqual(tables[offset + 268:offset + 276],
                                 b'MsHyperV' if enabled else bytes(8))
                self.assertEqual(manifest['hyperv'], enabled)

    def test_invalid_memory(self):
        for size in (0, 255, 16385):
            with self.assertRaises(ValueError):
                build(1, Path('/unused'), size)

    def test_invalid_count(self):
        for count in (0, 9):
            with self.assertRaises(ValueError):
                build(count, Path('/unused'))


if __name__ == '__main__':
    unittest.main()
