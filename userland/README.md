# userland

Traditional Unix userland: small, composable utilities on the native
libc (docs/userland/). `init/` (pid 1's job), `shell/` (`sh`, prompt
`cosmo$ `), `coreutils/` (`/bin`), `system/` (`/sbin`), `etc/` (`rc`,
`rc.test`), `networking/` (not started). `userland.mk` builds every
program as `crt0.o program.o libc.a` at 4 MiB (`user.ld`) and generates
the boot archive entries the kernel places at `/bin`, `/sbin`, `/etc`.
