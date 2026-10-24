"""Original fixtures: reject WinPE, inactive drivers and incomplete observations."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_windows_boot import verify


def complete_report():
    report = '''PCEMU_INSTALLED_WINDOWS_BOOT
Microsoft Windows [Version 10.0.26300.9457]
SYSTEMDRIVE=C:
PCEMU_FIRST_LOGON=PCEmu
CurrentBuildNumber    REG_SZ    26300
device                  partition=C:
path                    \\Windows\\system32\\winload.efi
osdevice                partition=C:
IPv4 Address. . . . . . . . . . . : 10.0.2.15(Preferred)
HTTP_GET_RC=0
PCEMU_WINDOWS_HTTP_OK
PCEMU_VIRTIO_RNG_PROVIDER_OK bytes=128
RNG_CHECK_RC=0
'''
    for identifier in ('1042', '1041', '1044'):
        report += f'\nInstance ID: PCI\\VEN_1AF4&DEV_{identifier}&SUBSYS_00411AF4\nStatus: Started\n\n'
    return report + 'PCEMU_INSTALLED_WINDOWS_REPORT_END\n'


class BootReportTest(unittest.TestCase):
    def test_complete_fixture(self):
        self.assertTrue(verify(complete_report())['verified'])
        self.assertTrue(verify(complete_report().replace('\n', '\r\n'))['verified'])

    def test_required_evidence_cannot_be_removed_or_substituted(self):
        changes = [
            ('PCEMU_INSTALLED_WINDOWS_BOOT', ''),
            ('PCEMU_INSTALLED_WINDOWS_REPORT_END', ''),
            ('26300', '26100'),
            ('PCEMU_FIRST_LOGON=PCEmu', ''),
            ('SYSTEMDRIVE=C:', 'SYSTEMDRIVE=X:'),
            ('partition=C:', 'ramdisk=[boot]\\sources\\boot.wim'),
            ('Status: Started', 'Status: Problem\nProblem Code: 10'),
            ('DEV_1044', 'DEV_9999'),
            ('HTTP_GET_RC=0', 'HTTP_GET_RC=1'),
            ('PCEMU_WINDOWS_HTTP_OK', ''),
            ('PCEMU_VIRTIO_RNG_PROVIDER_OK bytes=128', 'PCEMU_VIRTIO_RNG_PROVIDER_FAILED'),
            ('RNG_CHECK_RC=0', 'RNG_CHECK_RC=1'),
        ]
        for old, new in changes:
            with self.subTest(evidence=old):
                self.assertFalse(verify(complete_report().replace(old, new))['verified'])

    def test_duplicate_device_is_ambiguous(self):
        duplicate = '\nInstance ID: PCI\\VEN_1AF4&DEV_1042&OTHER\nStatus: Started\n\n'
        self.assertFalse(verify(complete_report() + duplicate)['verified'])


if __name__ == '__main__':
    unittest.main()
