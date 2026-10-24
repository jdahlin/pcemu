# ARM64 SMP

The HVF backend supports one to eight vCPUs with framework GICv3. CPU 0 stays on
the coordinator thread; secondary vCPUs are created sequentially on persistent
worker threads before the first guest instruction. MPIDR affinity 0 equals the
CPU index. All vCPU register APIs run on their owning thread.

Online CPUs run concurrently. A device/PSCI/debug exit cancels the other CPUs;
the coordinator waits until all are stopped before returning an exit for device
emulation. Concurrent exits are queued and consumed before any CPU resumes.
This preserves the existing devices' stopped-vCPU DMA contract. One CPU retains
the direct execution path. Worker threads remain allocated across CPU_OFF/ON;
the framework requires the GIC CPU topology to remain fixed after first run.

Secondary register state is captured on a trap and edits are applied by its
worker before reentry. Virtual-counter sleep corrections use the same offset on
all CPUs. CPU start resets EL1 execution state and passes context in X0.
The final online CPU cannot be taken offline through CPU_OFF.

`build/pcemu-hvf --self-test` checks a shared-memory rendezvous that requires
parallel execution, per-CPU contexts/MPIDR, invalid CPU starts, CPU_OFF and a
second CPU_ON. The signed backend suite passes 102 checks, including an idle-secondary
regression: cancellation targets are removed as each CPU exits, preventing a
watchdog from repeatedly canceling a CPU that already stopped. An injected
20-second host-sleep interval advances both virtual counters consistently.

## Configure the platform

`./run-debian.sh` defaults to two vCPUs. Use `DEBIAN_CPUS=1` for the original
one-CPU setup, or `DEBIAN_CPUS=4` (also `./run-debian.sh --cpus 4`). The supported
range is 1–8. The launcher generates matching topology assets before booting.

For direct runner use:

```sh
python3 tools/build_arm_platform.py --cpus 4
build/pcemu-hvf --machine arm64-uefi --cpus 4 --vars PRIVATE_VARS [other options]
```

Counts above one select `build/arm64-fw/cpusN/pcemu-virt.dtb` and its `acpi/`
directory by default. Explicit `--dtb`/`--acpi-dir` overrides must describe the
same topology. The runner rejects a mismatching MADT CPU count or affinity.

The generator extends the hash-checked DSDT namespace with CPU devices, emits
matching MADT entries and a PPTT containing one virtual package and N distinct
cores, and reserves N redistributor frames. It does not invent physical host
cache descriptions. Device tree and fw_cfg NB_CPUS/MAX_CPUS agree with the
configured count. No extra AML compiler is needed for normal generation; the
2- and 8-CPU DSDTs were also independently disassembled with ACPICA.

PSCI CPU_ON, CPU_OFF and AFFINITY_INFO use live CPU state. CPU_ON starts at the
validated physical entry with context in X0; successful CPU_OFF does not return
to that CPU until a later CPU_ON. The shared fallback still advertises PSCI 0.2.
`PCEMU_TRACE_PSCI=1` logs CPU_ON arguments/results for diagnosis, and `--cpu-state`
reports each CPU's last PC.

## Debian validation

Debian 13.7 boots with two and four CPUs. The four-CPU guest delivers timer and
rescheduling/function-call IPIs on every CPU with interrupt errors zero. Two
CPU-pinned 256 MiB SHA-256 workloads ran concurrently with equal digests and
increased user CPU accounting on both cores. Linux CPU 1 was taken offline and
online successfully through sysfs; the kernel reported CPU_OFF and a fresh
secondary boot. No services failed.

Ignored evidence is under `build/smp/`: `dmesg.log`, `concurrent-workload.log`,
`four-verification.log` and serial/host logs. These establish correctness, not
throughput scaling. Device emulation is still serialized, with one block queue,
one network RX/TX pair and legacy INTx; multiqueue/MSI-X and event-driven network
wakeups remain separate work.

The default two-CPU launcher also passed a full Debian reboot in one runner
process, preserving a newly written file, followed by a three-second process
pause. SSH recovered with two CPUs online, no failed services and wall/RTC time
matching the host at second resolution. Normal boot has no debug-shell option.
Evidence: `build/smp/lifecycle/reboot-pause-summary.json` (ignored).
