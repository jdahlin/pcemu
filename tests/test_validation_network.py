"""Offline Validation OS network inventory parsing, without proprietary media."""
import importlib.util
from pathlib import Path
import struct
import unittest

PATH = Path(__file__).resolve().parents[1] / 'tools/inspect_validation_network.py'
SPEC = importlib.util.spec_from_file_location('validation_network', PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NetworkInventory(unittest.TestCase):
    def test_inf_class_and_hardware(self):
        info = MODULE.inf_summary('''; Class=Wrong
[Version]
Class = Net ; adapter
ClassGUID={4d36e972-e325-11ce-bfc1-08002be10318}
Provider=%Vendor%
[Models.NTarm64]
%Name%=Install, PCI\\VEN_1AF4&DEV_1041,PCI\\VEN_1AF4
[Install.Services]
AddService=netkvm,2,Service
[Service]
ServiceBinary=%12%\\netkvm.sys
[Strings]
Adapter.DeviceDesc="Virtual Ethernet adapter"
''')
        self.assertEqual(info['class'], 'Net')
        self.assertEqual(info['hardwareIds'], ['PCI\\VEN_1AF4', 'PCI\\VEN_1AF4&DEV_1041'])
        self.assertEqual(info['services'], ['netkvm'])
        self.assertEqual(info['serviceBinaries'], ['%12%\\netkvm.sys'])
        self.assertEqual(info['descriptions'], ['Virtual Ethernet adapter'])

    def test_debug_adapter_and_framework_are_distinct(self):
        debug = MODULE.inf_summary('[Version]\nClass=Net\n[Models]\n%Name%=Install,root\\kdnic')
        framework = MODULE.inf_summary('[Version]\nClass=Generic\n[Models]\n%Name%=Install')
        self.assertEqual(debug['hardwareIds'], ['root\\kdnic'])
        self.assertEqual(framework['class'], 'Generic')
        self.assertEqual(framework['hardwareIds'], [])

    def test_unicode_and_utf8(self):
        source = '[Version]\nClass=Net\n; nätverk'
        self.assertEqual(MODULE.decode_inf(source.encode('utf-16')), source)
        self.assertEqual(MODULE.decode_inf(source.encode('utf-8-sig')), source)

    def test_pe_machine(self):
        data = bytearray(128)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 96)
        data[96:100] = b'PE\0\0'
        for value, name in ((0xaa64, 'ARM64'), (0x8664, 'AMD64'), (0x14c, 'I386')):
            struct.pack_into('<H', data, 100, value)
            self.assertEqual(MODULE.pe_architecture(data), name)
        struct.pack_into('<I', data, 60, 4096)
        with self.assertRaises(ValueError):
            MODULE.pe_architecture(data)
        with self.assertRaises(ValueError):
            MODULE.pe_architecture(b'not a PE')


if __name__ == '__main__':
    unittest.main()
