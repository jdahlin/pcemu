#!/usr/bin/env python3
"""Generate a private answer file for this local Windows validation VM.

Passwords are generated once, stored in a mode-0600 ignored local file, and
never printed. The tracked template contains no credentials.
"""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = 'urn:schemas-microsoft-com:unattend'
WCM = 'http://schemas.microsoft.com/WMIConfig/2002/State'
ET.register_namespace('', NS)
ET.register_namespace('wcm', WCM)


def validate(user, password):
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,19}', user) or user.lower() in ('administrator', 'guest', 'defaultaccount', 'wdagutilityaccount'):
        raise ValueError('Expected a new local test-account name')
    if len(password) < 20 or '\x00' in password:
        raise ValueError('Expected a generated password of at least 20 characters')


def answer_file(template, user, password):
    validate(user, password)
    root = ET.fromstring(template)
    def child(parent, name, text=None, attributes=None):
        item = ET.SubElement(parent, '{' + NS + '}' + name, attributes or {})
        item.text = text
        return item
    settings = child(root, 'settings', attributes={'pass': 'oobeSystem'})
    def component(name):
        return child(settings, 'component', attributes={'name': name, 'processorArchitecture': 'arm64',
                     'publicKeyToken': '31bf3856ad364e35', 'language': 'neutral', 'versionScope': 'nonSxS'})
    locale = component('Microsoft-Windows-International-Core')
    for key, value in (('InputLocale', '0409:00000409'), ('SystemLocale', 'en-US'), ('UILanguage', 'en-US'), ('UserLocale', 'en-US')):
        child(locale, key, value)
    shell = component('Microsoft-Windows-Shell-Setup')
    oobe = child(shell, 'OOBE')
    for key in ('HideEULAPage', 'HideOEMRegistrationScreen', 'HideOnlineAccountScreens', 'HideWirelessSetupInOOBE'):
        child(oobe, key, 'true')
    child(oobe, 'ProtectYourPC', '3')
    account = child(child(child(shell, 'UserAccounts'), 'LocalAccounts'), 'LocalAccount', attributes={'{' + WCM + '}action': 'add'})
    child(account, 'Name', user); child(account, 'DisplayName', user)
    child(account, 'Group', 'Administrators')
    def password_element(parent):
        node = child(parent, 'Password')
        child(node, 'Value', password); child(node, 'PlainText', 'true')
    password_element(account)
    autologon = child(shell, 'AutoLogon')
    child(autologon, 'Username', user); child(autologon, 'Enabled', 'true')
    child(autologon, 'LogonCount', '1'); password_element(autologon)
    command = child(child(shell, 'FirstLogonCommands'), 'SynchronousCommand', attributes={'{' + WCM + '}action': 'add'})
    child(command, 'Order', '1')
    child(command, 'Description', 'Verify local Windows desktop and VirtIO')
    child(command, 'CommandLine', 'cmd.exe /c start "" /b cmd.exe /c C:\\PCEMU\\FIRSTLOGON.CMD')
    ET.indent(root, space='  ')
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def write_private(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'wb') as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user', default='PCEmu')
    parser.add_argument('--credentials', type=Path, default=ROOT / 'media/windows11-arm64/local-account.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/windows11-install/account-unattend.xml')
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink():
        parser.error('Refusing to overwrite an answer file')
    if args.credentials.exists():
        account = json.loads(args.credentials.read_text())
        if account['username'] != args.user:
            parser.error('Existing credentials belong to a different account')
    else:
        account = {'username': args.user, 'password': 'Aa1!' + secrets.token_urlsafe(24)}
        validate(account['username'], account['password'])
        write_private(args.credentials, (json.dumps(account, indent=2) + '\n').encode())
    document = answer_file((ROOT / 'tools/windows/unattend.xml').read_bytes(), account['username'], account['password'])
    write_private(args.output, document)
    print(f'Private answer file: {args.output}\nLocal account: {args.user}\nCredentials file: {args.credentials}')


if __name__ == '__main__':
    main()
