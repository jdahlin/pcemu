# Win95 selected-title OCR fallback

The conditional-trace rtts validation reaches a visibly correct Calculator but
its text check times out: sparse and doubled whole-window OCR omit its selected
white-on-blue title and misread the taskbar label. The guest remains live.
[Actual captured image](measurements/win95-calculator-title-ocr-2026-10-26.png).

When an application text check is missing an expected token, the verifier now
finds actual blue horizontal bands (at least 64 blue pixels per row, 8–24 rows)
and reads bright title glyphs as black on white at 4× size. At most two bands
are read. Existing literal expected tokens remain required; no fuzzy app-name
acceptance is added. The ordinary OCR and desktop pixel gates remain in place.

On the captured failure, this reads `Calculator`; a plain teal desktop produces
no title text. Python compilation and whitespace checks pass. The same live rtts
guest resumes through Calculator 2+2, clipboard, Notepad save and restart after
adding the fallback. Exact stopped-guest FAT readback matches 38 bytes and the
private child is removed. The separate installed OSR2 trace-mode application/
restart/readback run also passes. The initial OCR miss remains recorded rather
than being called an emulator failure or an uninterrupted verifier run.

[Pixel-check results and source/image identities](measurements/win95-calculator-title-ocr-2026-10-26.json).
