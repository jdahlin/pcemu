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
second CPU_ON. The complete signed backend suite currently passes 85 checks.
Firmware topology, CLI configuration and Debian SMP validation are the next
integration layer; backend tests alone do not establish a multiprocessor OS boot.
