"""Known-window focus safety and launch retry, without running a VM."""
import ast
from pathlib import Path
from types import SimpleNamespace
import unittest

SOURCE = Path(__file__).resolve().parents[1]/'tools/check_win95_restart.py'


def functions(*names):
    tree=ast.parse(SOURCE.read_text())
    selected=ast.Module(body=[n for n in tree.body
        if isinstance(n,ast.FunctionDef) and n.name in names],type_ignores=[])
    namespace={}
    exec(compile(selected,str(SOURCE),'exec'),namespace)
    return namespace


class WelcomeTests(unittest.TestCase):
    def focus_case(self, titles):
        ns=functions('dismiss_delayed_welcome')
        calls=[]
        remaining=iter(titles)
        ns.update(a=SimpleNamespace(frame=SimpleNamespace(read_bytes=lambda:b'P6\n1 1\n255\nabc')),
            read_blue_titles=lambda *args:next(remaining),
            shortcut=lambda *args:calls.append(args))
        return ns,calls

    def test_inactive_welcome_is_focused_before_close(self):
        ns,calls=self.focus_case(['notepad','welcome'])
        ns['dismiss_delayed_welcome']()
        self.assertEqual(calls,[(0x38,0x0f),(0x38,0x3e)])

    def test_active_welcome_closes_directly(self):
        ns,calls=self.focus_case(['welcome'])
        ns['dismiss_delayed_welcome']()
        self.assertEqual(calls,[(0x38,0x3e)])

    def test_unrelated_window_is_never_closed(self):
        ns,calls=self.focus_case(['notepad','calculator','run','explorer'])
        with self.assertRaisesRegex(RuntimeError,'Could not focus'):
            ns['dismiss_delayed_welcome']()
        self.assertEqual(calls,[(0x38,0x0f)]*4)

    def test_run_request_is_reissued_after_delayed_welcome(self):
        ns=functions('wait_text')
        texts=iter(['welcometowindows','typethenamewindowswillopenit'])
        calls=[]
        ns.update(time=SimpleNamespace(monotonic=lambda:0,sleep=lambda _:None),
            observe=lambda **_:({},next(texts)),normalize=lambda s:s.replace(' ',''),
            dismiss_delayed_welcome=lambda:calls.append('dismiss'),
            shortcut=lambda *args:calls.append(args),
            send=lambda *args,**kwargs:calls.append(args))
        ns['wait_text']('type the name','windows will open it')
        self.assertEqual(calls,['dismiss',(0x1d,0x01),('text r',)])


if __name__=='__main__':
    unittest.main()
