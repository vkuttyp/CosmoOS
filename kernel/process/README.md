# kernel/process

Processes (docs/kernel/process/): `process.c` (the process object,
creation from an ELF image with spawn attributes, exit, zombies and
`process_wait_child`, `process_kill` and its delivery points,
reparenting to init, the working directory, `process_info`), `spawn.c`
(`process_spawn`: creation from an executable file on behalf of the
calling process, with a validated handle map), `elf.c` (the static ELF
validator and loader), `proctest.c` (the `elf`, `process-reject`,
`process-spawn`, `process-user`, `process-fault` self-tests). Threads
live in `kernel/scheduler/`; user-mode entry in `kernel/arch/`.
