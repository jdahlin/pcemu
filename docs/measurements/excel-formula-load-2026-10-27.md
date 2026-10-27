# Small Excel 97 formula workbook loading

The new headless test launches Excel 97 and opens `C:\FORMULA.XLS`, a
42,496-byte BIFF8 workbook containing 64 data rows and 650 live formulas across
Summary, Model and Inputs. It complements the existing already-open 4,096-row
recalculation test; the two timings have different scopes.

| Frozen engine | Median elapsed | Median process CPU |
| --- | ---: | ---: |
| Original baseline `57ce863` | 1,726 ms | 1,675 ms |
| Current native coverage `e5be4dd` | 1,320 ms | 1,279 ms |

Three alternating original/current pairs all passed. Current elapsed time is
23.5% below the original baseline and CPU time 23.6% below it. This establishes
a new workload baseline; no emulator optimization was introduced in this change.
The original/current elapsed ranges were 1,519–1,845 / 1,244–1,347 ms. Three
pairs suffice to establish the short workload, not to resolve small speedups.

The timer starts with Enter in the prepared Run dialog and ends when all ten
calculated summary results match exact framebuffer pixels. It includes cold JIT
compilation, Excel process launch, file loading, calculation, painting and
control/capture polling. Windows boot and snapshot restore are excluded. Each
run restores the same immutable checkpoint into a private small compressed disk
child. The Library VM was paused and resumed around the scored cohort, with no
profiling or build work overlapping it. Detailed samples, hashes and profiling
scope are in the adjacent JSON report; the task JSON contains the pixel gate.

## Formula correctness

The model uses cross-sheet references, VLOOKUP, IF, ROUND, a dependent running
total and SQRT. Summary uses SUM, SUMIF, AVERAGE, MIN, MAX, COUNT and COUNTIF.
Inputs are deterministic synthetic data. There are no macros or external links.
Independent arithmetic verifies Artifact Tool calculation before export, with
an error scan and rendered previews. The legacy exporter retains every formula
and verifies the BIFF formula count and calculate-on-open flags.

Excel 97 visibly calculated the ten expected results on opening:
`348232, 348232, 894, 34, 66.66, 18060, 136, 64, 384557, 64`.
Editing Inputs!B3 from 1 to 2 produced
`679552, 679552, 1788, 42, 92.90, 36120, 272, 64, 769114, 64`.
The root means are shown to two decimals; independent checks use full precision.
An independent restore with no input timed out, as required. Its owned guest
was then used for the scale-two correctness check and quit without changing the
fixture. First-run Windows Welcome UI was dismissed before the ready checkpoint.

An initial xlwt export displayed blank cached formula results. Setting each
BIFF formula's calculate-on-open bit fixed this in Excel 97. Sheet-level
automatic calculation and save-recalculation settings alone were insufficient.
Failed setup/pilot attempts are not counted in the six scored runs.

## Instruments

A separate current-engine Time Profiler run passed the same pixel gate.
Within its 1,129 main-thread task samples: dispatch/cache/validation 23.6%,
generated code/helpers 22.6%, compilation 21.2%, interpreter/MMU/faults 16.7%,
capture/video/control 9.2%. Disk/compression was only four samples. This short
launch test emphasizes compilation and interpreter coverage more than the
existing Excel recalculation test. Sample shares are approximate, not exact
function durations; profiled timing is excluded from the comparison.

## Reproduction

`tools/build_excel_formula_load.mjs OUTPUT_DIRECTORY [ROWS=64]` authors and
verifies the canonical XLSX with the bundled `@oai/artifact-tool` runtime.
The Node module must resolve that package (the local task uses a symlink under
`build/excel-formula-load` and `--preserve-symlinks-main`).
`tools/export_excel_formula_load.py DEFINITION_JSON OUTPUT_XLS --dependencies DIR`
uses xlwt 1.3.0, xlrd 2.0.2 and olefile 0.47 from that directory to export BIFF8.
Formula caches remain empty; Excel itself calculates on open.

Checks also covered a one-row export (20 formulas, complete 32-product lookup
table), Python compilation, Node syntax validation and a byte-identical rebuild
of the measured 64-row XLS. No production emulator code changed.

Stage into a private child of the existing `build/snapshots/excel-local/disk.raw`
using `build/disk-tool overlay`, `tools/cow_file.py` and `tools/fat_stage.py`.
Cold boot with `--machine win95 --ram-mb 64 --execute-vgabios --headless`, since
an offline FAT edit must not be resumed with stale Windows disk caches. Close
Welcome, prepare Run with `C:\FORMULA.XLS` awaiting Enter, and save a checkpoint
before launching Excel. The local ready checkpoint is
`build/snapshots/excel-formula-load-ready`.

Run `tools/bench_snapshot_task.py` with that checkpoint, the adjacent task JSON,
`--emulator` selecting the frozen engine, and a fresh `--output` directory.
Use `--instruments` only in a separate diagnostic run; analyze with
`tools/analyze_instruments.py`. Keep disks, workbooks, renders and traces under
ignored `build/`; do not commit or recreate large VM media.
