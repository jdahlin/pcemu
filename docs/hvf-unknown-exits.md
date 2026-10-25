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
