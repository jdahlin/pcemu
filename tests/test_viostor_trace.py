"""Original fixtures for WPP metadata and format correlation."""
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import inspect_viostor_trace as trace


class TraceTest(unittest.TestCase):
    def test_provider_and_message_pair(self):
        fixture = '''S_ANNOTATION strings = [
TMF:
12345678-1234-1234-1234-123456789ABC original.sys // SRC=fixture.c
#typev fixture_c1 7 "%0 original status %10!x!" // LEVEL=ERROR
{ value, ItemLong -- 10 }]
S_ANNOTATION strings = [
TMF:
abcdefab-1234-1234-1234-123456789abc other.sys // SRC=other.c
#typev other_c1 7 "other message" // LEVEL=INFO
{}]'''
        parsed = trace.formats(fixture)
        self.assertEqual(parsed[('12345678-1234-1234-1234-123456789abc', 7)], '%0 original status %10!x!')
        self.assertEqual(len(parsed), 2)

    def test_optional_header_sizes_and_truncation(self):
        for flags, count in ((0, 0), (2, 16), (0x2a, 32), (0x2b, 36), (0xaa, 32)):
            h = SimpleNamespace(event_property=flags, payload=b'X' * count + b'arguments')
            self.assertEqual(trace.message_data(h), b'arguments')
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            trace.message_data(SimpleNamespace(event_property=0x2a, payload=b'short'))
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            trace.message_data(SimpleNamespace(event_property=4, payload=b''))


if __name__ == '__main__':
    unittest.main()
