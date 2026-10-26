# Delayed Welcome recovery in unattended OSR2 validation

The installed OSR2 check reached Explorer but could not open Run when its
modeless Welcome window appeared late. The captured failure shows Welcome
with an inactive grey title while desktop focus is elsewhere. Repeated Escape
keys did not close it. An unchanged reference executable passed a control run;
this observation alone does not establish an emulator/MMU correctness defect.

The checker now cycles focus and reads the actual active blue title before
sending Alt+F4 to Welcome. It never closes an unidentified active window. It
then reissues the Run shortcut when Run was the requested target. Four unit
tests cover inactive/active Welcome, refusal to close unrelated windows and
Run retry. Target framebuffer verification and failure retention remain intact.

A new run of the same frozen MMU prototype exercised the delayed-Welcome
recovery, ran Calculator/clipboard/Notepad, restarted through firmware and
verified all 38 saved bytes after the guest stopped. The owned compressed disk
was removed after success. This uses an existing installed parent and does not
reinstall Windows. The prototype is private; this commit changes only checking.

[Control, failure and successful recovery evidence](measurements/win95-welcome-2026-10-26.json)
records executable identities and exact file hashes. The failed run's disk and
frame remain diagnostic artifacts. Its process was missing on subsequent
inspection; it was not restarted on the assumption that a timeout implied exit.
