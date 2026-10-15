# userland/init

PID 1 (docs/userland/). `init` prints its banner, sets `PATH` and
`HOME`, runs `sh /etc/rc` and waits, then runs `sh` on the console and
exits with the shell's status when it ends (the kernel treats init's
exit as the end of the boot: the single-shell bring-up policy). SIGUSR2,
which BusyBox `poweroff` sends to pid 1, ends it the same way from any
shell: a sync, then exit 0; the kernel then powers off where it can
(PSCI `SYSTEM_OFF` on AArch64). While
waiting it reaps every child, including orphans the kernel reparents to
it. Modes: `--selftest` exercises every native system call through libc
(`fs_selftest`, `net_selftest`, `proc_selftest` and the Phase 4 checks
in `init.c`) and reports `USERTEST: PASS`/`FAIL`; `--crash` faults on
purpose; `--block` reads the console and `--spin` loops, both for the
kernel's kill test. Delivered as the `init` entry of the boot archive,
visible at `/boot/init`.
