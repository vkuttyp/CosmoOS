# userland/coreutils

`/bin`: `echo` `cat` `ls` `cp` `mv` `rm` `mkdir` `rmdir` `pwd` `true`
`false` `sleep`, one file each, on libc (docs/userland/api.md). Options
are parsed by hand and precede operands; errors read `name: path:
reason`; exit 0, 1 on failure, 2 for usage.
