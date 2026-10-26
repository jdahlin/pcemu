# Repository instructions

Commit after every change. Do not leave completed changes uncommitted.

- Include the code, tests, and documentation belonging to the change in the commit.
- Run the relevant checks before committing and describe the change clearly in the commit message.
- Respect `.gitignore`; keep build output, installation media, and local VM disks out of Git.

- Run x86 guest benchmarks and automated validation headless unless the user explicitly requests a GUI run.

- Keep x86 performance validation disk-light: prefer RAM-only kernels and existing guest images with small temporary compressed overlays removed after success. Do not recreate large Office or VM fixtures unless the user explicitly requests them.

- For new or migrated device code, follow [the specification-oriented device convention](docs/device-specification-style.md).
