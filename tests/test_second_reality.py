"""Regression for completion detection at instruction-batch text boundaries."""
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from smoke_second_reality import completion_verified

class CompletionTests(unittest.TestCase):
    def test_partial_status_is_never_completion(self):
        for ending in ['SECOND REALITY END','SECOND REALITY END P','SECOND REALITY END PAS']:
            self.assertFalse(completion_verified('closing screen\n'+ending+'\n'))
    def test_pass_requires_later_final_marker(self):
        self.assertFalse(completion_verified('SECOND REALITY END PASS\n'))
        self.assertFalse(completion_verified('SECOND REALITY FINISHED\nSECOND REALITY END PASS\n'))
        self.assertTrue(completion_verified('closing screen\nSECOND REALITY END PASS\nSECOND REALITY FINISHED\n'))
    def test_failure_and_conflicting_status_cannot_pass(self):
        self.assertFalse(completion_verified('SECOND REALITY END FAIL\nSECOND REALITY FINISHED\n'))
        self.assertFalse(completion_verified('SECOND REALITY END PASS\nSECOND REALITY END FAIL\nSECOND REALITY FINISHED\n'))

if __name__=='__main__':unittest.main()
