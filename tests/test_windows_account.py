"""Private unattended-account generation without proprietary media or secrets."""
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_windows_account as account


class AccountTest(unittest.TestCase):
    def test_answer_file_and_single_autologon(self):
        template = b'<unattend xmlns="urn:schemas-microsoft-com:unattend"><settings pass="specialize" /></unattend>'
        secret = 'Aa1!fixture&not-a-real-password'
        raw = account.answer_file(template, 'PCEmu', secret)
        root = ET.fromstring(raw); ns = {'u': account.NS}
        self.assertIsNotNone(root.find('u:settings[@pass="specialize"]', ns))
        shell = root.find('u:settings[@pass="oobeSystem"]/u:component[@name="Microsoft-Windows-Shell-Setup"]', ns)
        self.assertEqual(shell.find('u:AutoLogon/u:LogonCount', ns).text, '1')
        self.assertEqual(shell.find('u:AutoLogon/u:Password/u:Value', ns).text, secret)
        self.assertEqual(shell.find('u:UserAccounts/u:LocalAccounts/u:LocalAccount/u:Name', ns).text, 'PCEmu')
        self.assertIn('FIRSTLOGON.CMD', shell.find('u:FirstLogonCommands/u:SynchronousCommand/u:CommandLine', ns).text)
        self.assertNotIn(b'SkipMachineOOBE', raw)
        self.assertEqual(len(root.findall('.//u:Password', ns)), 2)

    def test_private_files_are_exclusive_and_mode_0600(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'private' / 'credentials'
            account.write_private(path, b'original fixture')
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError):
                account.write_private(path, b'replacement')
            self.assertEqual(path.read_bytes(), b'original fixture')

    def test_reserved_or_unsafe_account_names_are_refused(self):
        for name in ('Administrator', 'Guest', '../user', 'DOMAIN\\user', 'x' * 21):
            with self.subTest(name=name), self.assertRaises(ValueError):
                account.validate(name, 'Aa1!fixture-not-a-real-password')
        with self.assertRaises(ValueError):
            account.validate('PCEmu', 'short')


if __name__ == '__main__':
    unittest.main()
