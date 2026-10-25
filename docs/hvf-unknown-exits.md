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
