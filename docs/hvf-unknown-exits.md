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
