# JIT arena capacity: no default change

The [emitted-code profiles](emitted-code-profiling.md) show about 24 MB of native
code with an enlarged diagnostic arena. This prompted an uninstrumented test
of a 32 MiB arena against the production 16 MiB setting. The frozen executables
differ only in the `main.cpp` JIT constructor's capacity. Translation policy,
code generator, task inputs and expected result pixels are unchanged.

Three alternating headless pairs per workload reuse the existing external
checkpoints. Every result pixel gate passes. Restore is outside task timing;
process CPU still includes task observation/capture/control overhead. Other
desktop work is not stopped.

| Workload | CPU ratios, baseline/candidate | Median CPU ratio | Median response ratio |
| --- | --- | ---: | ---: |
| Excel dependent calculation | 1.036, 0.975, 0.933 | 0.975 | 0.972 |
| Word warm launch | 1.026, 1.041, 1.027 | 1.027 | 1.041 |

Ratios above one favor 32 MiB. Excel does not consistently benefit; Word favors
32 MiB slightly. Compilation counts fall substantially in both: Excel from
about 31,900 to 22,200 task blocks, Word from 37,800–39,400 to about 26,500.
Reduced compilation does not establish proportional useful-work improvement,
especially in loaded Excel where compilation is a small sampled cost.

Keep the 16 MiB production default. These three-pair shared-host trials do not
prove that 32 MiB inherently slows Excel or that another capacity is optimal.
They do establish that the favorable large-arena diagnostic timing must not be
reported as a stable production gain. No extra configuration option or runtime
change is retained. No candidate-specific OS smoke/fresh Setup gate is claimed.

[Exact source difference, build/binary identity, drivers and every pair](measurements/jit-cache-capacity-2026-10-26.json).
No new guest disk or Office installation is made. Successful task overlays are
removed by the existing harness. The broader performance goal remains active;
finer attribution within emitted block finishing is the next supported lead.
