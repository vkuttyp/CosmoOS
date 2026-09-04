# kernel/tty

The line discipline (docs/kernel/tty/): `tty.c` collects bytes from a
device in interrupt context (`tty_input`, fed by the UART receive
interrupt in `kernel/arch/x86_64/serial.c`), edits them into lines
(erase, kill, `^D`, CR to NL, echo), and delivers one record per
killable `tty_read` to readers through the console kobject
(`kernel/object/console_obj.c`). One console tty; `ttytest.c` is the
`tty-ldisc` self-test.
