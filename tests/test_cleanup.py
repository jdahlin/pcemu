import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('cleanup', Path(__file__).resolve().parents[1]/'tools/clean_workspace.py')
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)


class CleanupSafety(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, size=1):
        p = self.root/name
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open('wb') as f:
            f.truncate(size)
        return p

    def selected(self, **kwargs):
        remove, compress = cleanup.plan(self.root, **kwargs)
        return {str(p.relative_to(self.root)) for p, _ in remove}, {str(p.relative_to(self.root)) for p, _ in compress}

    def test_snapshots_and_boot_disks_survive(self):
        for p in ['build/snapshots/point/state.bin', 'build/snapshots/point/disk.raw', 'media/dos.raw', 'media/win311.raw', 'media/win95.raw', 'media/rtts-win95.img']:
            self.file(p, 2*cleanup.MIB)
        self.assertEqual(self.selected(obsolete_media=True), (set(), set()))

    def test_original_archives_survive(self):
        for name in ['media/original.7z', 'media/original.zip', 'media/rtts-original.img']:
            self.file(name)
        self.assertEqual(self.selected(obsolete_media=True), (set(), set()))

    def test_referenced_debug_disk_survives(self):
        parent = self.file('media/rtts-v5.raw')
        child = self.file('build/snapshots/point/disk.raw')
        name = str(parent).encode()
        child.write_bytes(b'PCEMUCOW'+struct.pack('<IIQI', 1, 4096, 8, len(name))+name+b'\0'*8)
        self.assertEqual(self.selected(obsolete_media=True)[0], set())

    def test_missing_parent_stops_cleanup(self):
        child = self.file('build/snapshots/point/disk.raw')
        name = str(self.root/'missing.raw').encode()
        child.write_bytes(b'PCEMUCOW'+struct.pack('<IIQI', 1, 4096, 8, len(name))+name+b'\0'*8)
        with self.assertRaises(ValueError):
            self.selected()

    def test_tracked_and_symlink_files_survive(self):
        self.file('build/win311-smoke-demo/disk.raw')
        (self.root/'media').mkdir()
        (self.root/'media/rtts-link.raw').symlink_to(self.root/'build/win311-smoke-demo/disk.raw')
        self.assertEqual(self.selected(obsolete_media=True, tracked=['build/win311-smoke-demo/disk.raw'])[0], set())
        self.assertEqual(self.selected(obsolete_media=True)[0], set())

    def test_verified_ram_removed_unknown_binary_compressed(self):
        self.file('build/known.bin', 2*cleanup.MIB)
        self.file('build/ram-legacy.bin', 2*cleanup.MIB)
        self.file('build/known.bin.json').write_text(json.dumps({'cr0': 16, 'registers': [0]*8}))
        self.assertEqual(self.selected(), ({'build/known.bin'}, {'build/ram-legacy.bin'}))

    def test_generated_program_is_not_a_ram_dump(self):
        self.file('build/guest-perf/program.bin', 2*cleanup.MIB)
        self.file('build/new-kernel.bin', 32*cleanup.MIB)
        self.assertEqual(self.selected(), (set(), set()))

    def test_only_named_old_media_is_selected(self):
        for p in ['media/rtts-v4.raw', 'media/win95-install.raw', 'media/my-work.raw']:
            self.file(p)
        self.assertEqual(self.selected()[0], set())
        self.assertEqual(self.selected(obsolete_media=True)[0], {'media/rtts-v4.raw', 'media/win95-install.raw'})

    def test_smoke_disk_selected_diagnostics_retained(self):
        self.file('build/win311-smoke-demo/disk.raw')
        self.file('build/win311-smoke-demo/emulator.log')
        self.assertEqual(self.selected(), ({'build/win311-smoke-demo/disk.raw'}, set()))


if __name__ == '__main__':
    unittest.main()
