"""Keep emulated hardware independent of board and execution implementations."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1] / 'src'
DEVICES = (
    'ata', 'pic', 'i8259_pair', 'pit', 'vga', 'vga_pci', 'pci', 'ne2000',
    'i8042', 'mouse', 'cmos_rtc', 'system_port_b', 'port92', 'debug_console',
    'disk', 'network', 'virtio_blk', 'virtio_net', 'virtio_rng', 'virtio_pci',
    'nvme', 'arm_pl011', 'arm_pl031', 'arm_nor_flash', 'arm_fw_cfg', 'arm_ramfb',
    'arm_psci', 'arm_os_lock', 'io_port_space', 'mmio_space', 'pci_ecam',
)
FORBIDDEN = {'machine.hpp', 'arm_virt.hpp', 'cpu.hpp', 'jit.hpp', 'hvf_machine.hpp',
             'frontend.hpp', 'pc_bios_disk.hpp'}


class DeviceBoundaries(unittest.TestCase):
    def test_hardware_dependency_graph(self):
        def inspect(path, chain):
            if path.name in FORBIDDEN:
                self.fail('Hardware imports board/execution/frontend: ' + ' -> '.join(chain + [path.name]))
            if path.name in chain or not path.exists():
                return
            source = path.read_text()
            self.assertIsNone(re.search(r'\b(?:class|struct)\s+(?:Machine|CPU|Jit|HvfMachine|ArmVirtMachine)\b', source),
                              'Device forward-declares an execution/board type: ' + str(path))
            includes = re.findall(r'^\s*#\s*include\s*"([^"]+)"', source, re.MULTILINE)
            for include in includes:
                inspect(ROOT / include, chain + [path.name])
        for device in DEVICES:
            with self.subTest(device=device):
                self.assertTrue((ROOT / (device + '.hpp')).exists())
                for extension in ('.hpp', '.cpp'):
                    inspect(ROOT / (device + extension), [])


if __name__ == '__main__':
    unittest.main()
