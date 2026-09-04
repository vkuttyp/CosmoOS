# userland/init

PID 1 and service supervision. Today `init` prints a banner and exits;
with `--selftest` it exercises every native system call (`selftest()`
and `fs_selftest()` in `init.c`: files and directories on ramfs, then
`mount("vda", "/mnt", "cosmofs", 0)` to read what the kernel self-tests
left on the scratch disk) and reports `USERTEST: PASS`/`FAIL`; with
`--crash` it faults on purpose. Built by `userland/userland.mk`, linked
at 4 MiB (`user.ld`), delivered as the `init` entry of the boot archive
and also visible at `/boot/init`.
