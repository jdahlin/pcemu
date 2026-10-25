# Diagnosing unknown native HVF exits

The ARM runner preserves the native Hypervisor.framework exit reason and CPU
index. Unknown exits also capture PSTATE on the owning vCPU thread. The fatal
message adds the selected CPU's EL1 context, SCTLR, TTBR0/1 and TCR while all
vCPUs are stopped; unavailable optional registers retain the original failure.
No retry or guest execution policy changes are introduced.

The previous Windows clock experiment stopped at an HTTP body validation error
before changing the clock. Its later unknown native exit remains unexplained;
it is not evidence of a clock adjustment or a Hyper-V shutdown.

Validation: `make test-arm-hvf-gic` passes 60 checks, including preservation of
native exception reasons on both CPUs. Reproduction of the unknown Windows exit
with the additional diagnostic fields remains pending.

## Four-GiB Windows run

The diagnostic runner was relaunched with 4096 MiB RAM and two CPUs after
generating matching assets with:

```sh
python3 tools/build_arm_platform.py --cpus 2 --ram-mb 4096 --hyperv --vmbus --output build/arm64-fw/cpus2-ram4096-hyperv-vmbus
```

The initial launch failed before guest execution because those assets were
missing. The subsequent live capture confirms `ramMiB=4096`. Its audited
snapshot has seven heartbeat replies and seven timesync acknowledgments, all
three integration channels ready, zero failures and zero unsupported calls.
Core usage, synthetic timer delivery, IPIs, context synchronization and the
reference counter pass their observation gates. Evidence is retained under
`build/windows11-hyperv/unknown-exit/` (`host.log`, `hyperv.json`, `audit.json`,
`launch.json`); the audit includes report and off-mode baseline hashes.

This is startup evidence, not a long-duration stability result. The unknown
exit has not yet recurred; actual clock correction remains unverified. The
runner's separate native self-test passes all 284 checks.

The four-GiB run subsequently accepted an explicit Hyper-V shutdown request
and reached terminal `state=power-off` without reproducing the unknown exit.
The detached launch did not retain its process exit code, so no new complete
shutdown audit is claimed from that run.

The corrected clock probe (`276eaa3`) was staged through WinPE at four GiB.
The new staging report contains `PCEMU_PROBE_LOGON_DOMAIN_OK` and
`PCEMU_SERVICE_STAGE_OK`. Temporary secret-bearing WIM, boot disk, staging
overlay and generated credential command were removed after WinPE stopped.
The next installed-Windows run uses the same private test overlay, four GiB
and VMBus. Its `clock-adjustment/launch3.json` records the probe hash; the
supervisor saves the actual runner exit code to `exit3.json`. This staging
result does not establish clock adjustment; a fresh guest report is required.

## Typed VMBus publication evidence

VMBus now counts successfully published control replies separately for contact,
channel offers, all-offers-delivered, unload and channel lifecycle replies. A
blocked sender increments none of these counters. The Windows auditor accepts
active channel traffic only when all five typed counters are present, their
sum matches total delivery, each bootstrap category matches its accepted
requests/offers, and no channel fault is reported. Partial, negative, boolean
or inconsistent typed counters fail the gate. Older snapshots retain the
conservative pre-channel bootstrap check. SynIC slot consumption remains a
separate check; publication alone never proves consumption.

Validation: 281 VMBus channel checks pass under AddressSanitizer/UBSan, including
backpressure and all reply categories; 21 Windows auditor tests pass, including
live-channel and inconsistent-counter cases. The already-running clock guest
uses the preceding binary, so actual Windows typed-counter validation requires
a subsequent launch.

## Actual VMIC wall-clock correction

The corrected probe returned `clock-adjustment/guest-report3.json` from the
four-GiB Windows guest. Its initial UTC offset was 25,200.989 seconds; the
explicit backward step changed that to 25,079.574 seconds (a 121.415-second
observed decrease, including request timing). After vmictimesync restarted,
the fourth five-second sample reached an offset of 0.867259 seconds with
120.687 ms HTTP RTT. Windows reported `VM IC Time Synchronization Provider`
with exit code zero. NTP was disabled before and after correction,
`adjustmentObserved=true`, `restoredByProbe=false`, error was null and cleanup
errors were empty. The preexisting seven-hour clock offset is recorded as
observed; its cause has not been established.

The correlated immutable `clock-success-hyperv3.json` snapshot reports two
timesync negotiations, 20 samples and 20 acknowledgments, no rejected replies
or timeouts, and zero Hyper-V failures. `launch3.json` retains SHA-256 hashes
of both reports. This is actual guest wall-clock correction evidence, unlike
the earlier protocol acknowledgments alone. The command-line auditor's
`guestClockAdjustmentVerified` remains conservative until a dedicated input
validator is added; this observation does not change that gate implicitly.

## Automated guest-clock evidence gate

The auditor now accepts `--clock-report FILE`. It requires verified Windows
core/timesync use, an explicit measured backward step of approximately two
minutes, UTC timestamps consistent with reported offsets, ordered bounded
samples, a final offset within three seconds with RTT at most two seconds,
VMIC as the successful Windows time source, NTP isolation, and no probe
restoration, diagnostic error or cleanup failure. All .NET round-trip timestamp
fractions are parsed explicitly, including seven-digit Windows values.

The report is bounded to one MiB and hashed alongside the Hyper-V and baseline
inputs. Supplying a report makes failed clock verification a nonzero CLI exit.
Without it, the clock gate stays false. The actual third-run artifacts pass
this gate in `clock-adjustment/audit-clock3.json`; 22 auditor tests pass.

```sh
python3 tools/verify_windows_hyperv.py build/windows11-hyperv/clock-adjustment/clock-success-hyperv3.json --baseline build/windows11-hyperv/off/hyperv.json --clock-report build/windows11-hyperv/clock-adjustment/guest-report3.json
```
