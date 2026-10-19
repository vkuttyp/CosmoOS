# Userland: design

## Build and delivery

`userland/userland.mk` builds every program the same way:

```text
$(OUT)/userland/<name>.elf : crt0.o <objects> libc.a   linked with -T userland/user.ld (shared script; since Phase 11 the ELF and
                                                       program headers are inside the text segment and .note.cosmo sits in a PT_NOTE)
USER_CFLAGS = --target=x86_64 $(COMMON_CFLAGS) -fno-pic -fno-pie -ffreestanding? no:
              (no -mgeneral-regs-only: user code uses the FP/SIMD registers, which the kernel saves per thread)
              -nostdinc is NOT used (compiler builtin headers are wanted) but -nostdlib and
              -I libc/include -I kernel/include (for uapi/) -isystem <clang resource dir> are.
```

Programs are listed once (`USER_PROGRAMS := init sh echo cat ls ...`
with a directory per family); the archive entries are generated from
the list: `bin/<name>=$(OUT)/userland/<name>.elf`, `sbin/<name>` for
the system family, `etc/rc=userland/etc/rc`, and `etc/rc.test` only
when `SELFTEST=1`. `init` keeps its `init=` entry (the kernel finds it
by that name) and is also visible at `/boot/init`.

The kernel's boot namespace (`ramfs_populate_boot`) creates `/bin`,
`/sbin`, `/etc` and places archive entries whose first component is
one of those, or `usr`, there: mode 0755 for `bin/`, `sbin/`,
`usr/bin/`, `usr/sbin/` and `usr/libexec/` (the last since the scp-sftp
unit, for `/usr/libexec/sftp-server`), 0644 for `etc/` and the rest of
`usr/`; all other entries go under `/boot` as before. The rule is the
bootstrap namespace policy of the archive (an initramfs-like
convention), documented in `docs/kernel/module/` (the archive) and
`docs/kernel-services/vfs/` (ramfs).

## BusyBox (roadmap M3)

The Linux userland is static musl BusyBox, built from source with the
system (`ports/busybox/README.md`). `/bin/busybox` and one symbolic
link per applet of `ports/busybox/applets` -- the 48 of M3's decision
7, `/bin/sh` among them, M4's `wget`, `nslookup`, `stty` and `tty`, and
the later units' (S1's `ifconfig`, `route`, `netstat`, `arp` and
`hostname`, S2's 33 everyday applets, S3's `ps` and `kill`, S4's `chown`, `chgrp` and `su`, S5's
`reboot`, `halt`, `hwclock` and `ntpd`, S6's `mount` and `umount`) -- come
from the boot archive
(`mkbootarchive.py`'s `NAME@TARGET` entries, ustar type 2; the kernel
makes them with `vfs_symlink` in `ramfs_populate_boot`). A native program
whose name an applet takes is installed as `/bin/cosmo-<name>`
(`native_bin_name` in `userland/userland.mk`): the native shell is
`/bin/cosmo-sh`, and `cosmo-echo`, `cosmo-cat`, `cosmo-ls`, `cosmo-cp`,
`cosmo-mv`, `cosmo-rm`, `cosmo-mkdir`, `cosmo-rmdir`, `cosmo-pwd`,
`cosmo-true`, `cosmo-false` and `cosmo-sleep` likewise; `mkfifo`, `sync`
and the `/sbin` programs keep their names (`/sbin/hostname` beside
BusyBox's `/bin/hostname`, which `PATH` finds first and which also takes
`-s`, `-F` and `-f`; likewise `/sbin/ps` and `/sbin/kill` beside
BusyBox's, which since shell completeness S3 are what `ps` and `kill`
run: `ps` reads `/proc/<pid>/stat` and `status`, and shows sizes and
states the native one does not; and since S6 `/sbin/mount` and
`/sbin/umount` beside BusyBox's, whose `mount -t TYPE SOURCE DIR` is what
`mount` runs, while the native `mount SOURCE DIR TYPE` stays for the
scripts that name `/sbin/mount`). init's self-tests and the
native test scripts name the `cosmo-` programs, because what they test
is the native side.

**The console shell** is the path `/etc/console-shell` names, else
`/bin/cosmo-sh` (`console_shell` in init). `cosmo-install` writes
`/bin/sh` there, so an installed system's console -- and the shell that
runs its `/etc/rc` -- is BusyBox ash, while the boot image keeps the
native shell its interactive harness drives. init starts the console
shell -- not `/etc/rc` -- with `spawnve_session` (`COSMO_SPAWN_SETSID`):
it leads a session whose controlling terminal is the console, its group
in the foreground, so ash's job control (`ASH_JOB_CONTROL`, on since
roadmap M4: `jobs`, `fg`, `bg`, `^Z`) finds the terminal its own; `/etc/rc`
runs without, so what it starts in the background does not hear the
console hang up when rc exits. init gives the shell
`PATH=/bin:/sbin:/usr/bin:/usr/sbin`, `HOME=/root` and `PS1=cosmo$ `, so
either shell shows the prompt the harnesses wait for. `HOME` was `/`
until shell completeness S2; it is root's home now, as `/etc/passwd` and
an ssh session name it, so ash keeps its history in
`/root/.ash_history` (each command as it is entered,
`FEATURE_EDITING_SAVE_ON_EXIT` being off) and `cd` alone goes there. init
makes `/root` (0700) after choosing the root, since the live image's
ramfs root has none. `/etc/rc` runs
`/etc/rc.test` with `cosmo-sh`, and `rc.test` runs its inner shells as
`cosmo-sh -c`: it is the native shell's test. Its commands (`cat`, `ls`,
`cp`, ...) are BusyBox's now.

## init

```text
main(argc, argv):
  --selftest → selftest(); exit(failures ? 1 : 0)        (as before, now on libc)
  --crash    → *(volatile int *)0 = 1
  --block    → pipe(h); read(h[0], &c, 1); exit(5)        (a killable read nothing but the kill ends, for the kernel test)
  --spin     → for (;;) ;                                 (a CPU-bound loop, for the kernel test)
  print "init: CosmoOS userland, pid N"                   (N is 1 only when no self-tests ran first)
  setenv PATH=/bin:/sbin:/usr/bin:/usr/sbin HOME=/root; mkdir /root (after the root is chosen)
  sigaction(SIGUSR2): print "init: power off requested"; sync(); _exit(0)
                                                          (BusyBox poweroff's signal; in the handler,
                                                           so it cannot fall between a check and the wait)
  sigaction(SIGTERM): print "init: restart requested"; sync(); SYS_reboot(COSMO_REBOOT_RESTART)
  sigaction(SIGUSR1): print "init: halt requested"; sync(); SYS_reboot(COSMO_REBOOT_HALT)
                                                          (BusyBox reboot's and halt's, shell completeness
                                                           S5; only PRIV_SYS_ADMIN may send any of the
                                                           three, security S20)
  if stat("/etc/rc") ok: pid = spawnvp("/bin/sh", {"sh", "/etc/rc"}); waitpid(pid); print "init: rc exited N"
  for (;;)
      pid = spawnvp("/bin/sh", {"sh"}, NULL, 0)          (inherits 0,1,2)
      if pid < 0: print error; exit(1)
      loop: w = waitpid(-1, &st, 0)                        (reaps orphans too)
            if w == pid: break
      print "init: shell exited with status N"
      exit(st)                                             (single-shell bring-up policy)
```

### The disk root

Roadmap M2. Before `/etc/rc`, init reads `kernel.root` and
`kernel.rootdev`. Without `root=` nothing happens: the live system. With
it, init mounts `kernel.rootdev` as cosmofs at `/sysroot`, calls
`cosmo_switch_root("/sysroot")` (retrying `-EBUSY` for up to a second, a
process the kernel started may still be leaving), changes to `/`, mounts
a fresh ramfs on `/tmp` with `COSMO_MOUNT_SCRATCH`, which makes its root
01777 (a ramfs's root is otherwise 0755; uid 1000 writing there is
`test-install`'s check), and prints `init: switched to the disk root DEV
(root=SPEC)`. `/etc/rc` and the shell are then the installed system's.
Every failure -- `root=` naming no device, a mount refused, the switch
refused -- prints one line ending `staying on the live root`, undoes what
it did, and the boot continues live. When the shell exits on a disk root,
init calls `sync` and prints `init: disk root synced` before it returns.

`init --selftest` grows checks for every new call (`proc_selftest` in
`init.c`; the full list is in `testing.md`): pipes with `dup`/`dup2`,
EOF and `EPIPE`; `spawn` of `echo` into a pipe and its status; `sh -c
"cd /tmp && pwd && exit 7"` for cwd inheritance and the status; a `cat`
blocked on a pipe killed with `SIGKILL` (137); `chdir`/`getcwd` with
`..` normalisation, `ENOTDIR`, `ENOENT`, `ERANGE`; `getppid() == 0`;
its own `procinfo` record; `klog_read`; `sysctl_get`; `fstat(0)` is a
character device and `isatty(0)`; and the hostile spawn requests (a
closed parent handle `EBADF`, a duplicate child slot `EINVAL`, a
non-executable file and a directory `EACCES`, a missing file `ENOENT`,
an empty `argv` `EINVAL`), `waitpid` with no children `ECHILD`,
`kill(999999)` `ESRCH`, `kill(pid, 0)` `EINVAL`, `dup2(h, 64)` `EINVAL`.

## The shell

### Structure (`userland/shell/sh.c`, one file)

```text
main           options (-c, -e), interactive loop (read(0) per line, prompt on handle 2) or script loop (fgets)
lex            line → tokens: WORD (quotes and escapes handled per character, $ expanded while lexing),
               PIPE, SEMI, AND_IF, OR_IF, LESS, GREAT, DGREAT, GREAT2 ("2>"), GREAT2AND ("2>&1"), END
expand_dollar  $VAR ${VAR} $? $$ $0-$9 $#; no splitting of results (recorded gap)
parse_pipeline tokens → struct pipeline { struct command cmds[16] }; command → words + redirs
run_line       walks the token list: pipelines separated by ; && || with skip logic, -e handling
               (AND-OR lists are LEFT-associative -- see below)
run_pipeline   assignments alone → variables; a lone builtin runs in-process (redirected through dup/dup2
               when it has redirections); otherwise spawn each stage with a three-entry handle map
builtin        cd pwd exit export unset set : true false wait . source
var_*          shell variables (a flat array of 64); export moves them to the environment
```

### AND-OR lists are left-associative

`A && B || C` is `(A && B) || C`, so a failing `A` skips `B` and then
**runs `C`**. `run_line` keeps one `skip` flag and recomputes it at
every operator from `g_last_status`, the status of the last pipeline
actually run -- which is the list's accumulated status, because a
skipped pipeline does not change it.

Recomputing at *every* operator is the whole of it, and the first
version guarded the recomputation with `if (!skip)`: once `&&` had set
the flag, the following `||` never looked at it, and `A && B || C` with
a failing `A` ran nothing at all. `/etc/rc.test`'s own verdict line is
that shape, so a failing run printed no verdict -- `SHTEST: FAIL n` was
unreachable and only the absence of `SHTEST: PASS` reported it. It was
found while proving that `thrtest` propagates its exit status, which is
what makes `FAILS` non-zero in the first place. `rc.test` now asserts
both branches of both shapes, in two-term lists that mean the same
thing under either parse.

### Running a pipeline

```text
run_pipeline(pl):
  nstage = pl->n; pids[nstage]
  prev_read = -1
  for i in stages:
     map = { {0, prev_read or 0}, {1, stdout}, {2, 2} }         (child slots 0,1,2 always given)
     if i < nstage-1: pipe(p); map[1].parent = p[1]; next_read = p[0]
     apply redirections: open files in the parent, override map entries; 2>&1 → map[2].parent = map[1].parent
     pids[i] = spawnvp(argv[0], argv, map, 3)
     close in the parent: the pipe ends and redirection files just passed (the child has its copies)
     prev_read = next_read
  for i: waitpid(pids[i], &st); $? = st of the last stage
```

Because the child receives exactly the mapped handles, the parent closes
its copies immediately after `spawn` and pipes reach EOF correctly. A
failed `spawn` (command not found) prints `sh: name: not found`, the
stage's status is 127, and the other stages still run (they see EOF).

Builtins that must affect the shell (`cd`, `export`, `exit`, ...) run
in-process only when they are the sole command; in a pipeline they run
in-process too (there is no fork), with their input/output redirected
by temporarily `dup2`-ing the shell's handles and restoring them: this
is what makes `pwd > file` work.

### Jobs, the terminal, and ^C

An interactive shell runs the terminal. At start-up `job_control_init`
does three things, and gives up on any of them failing -- a shell
started inside another shell's job has no terminal to take, and
commands still run, they just share the shell's group and its signals:

1. `setsid()`, unless it already leads a group. A session of its own is
   what makes this terminal *this* shell's.
2. `tcsetpgrp(0, getpgrp())`, which claims the terminal for the shell's
   own group.
3. `signal(SIGINT, SIG_IGN)` and the same for `SIGQUIT`. The shell is
   about to make each job the foreground group, and it has to still be
   here to print a prompt when the job dies of the interrupt.

Then every pipeline is one job and therefore one process group: the
first stage is spawned with `spawnvp_pgrp(..., 0)` -- a group named by
its own pid -- and the rest name that group, so `^C` interrupts the
whole pipeline rather than whichever stage happened to be reading. The
shell hands the terminal to the job with `tcsetpgrp` before waiting and
takes it back afterwards; both calls may fail on a job that has already
exited, and neither failure changes what happens next.

(Until job control existed, nothing observable distinguished a per-job
group from the shell's own group. It does now: a background job is in a
group that is not the terminal's foreground one, which is what stops it
from reading the line the shell is waiting for.)

Worth saying plainly about the original signals unit: with no background
jobs, **nothing observable distinguished a per-job group from the
shell's own group** -- with
either, the terminal's foreground group contains the job, and `^C`
reaches it. The per-job group is here because it is what the group model
is for, and because the moment `&`, `fg` or `bg` exists the shell's
group and the job's must differ. The mechanism itself is tested on its
own (`signal-group`); the shell's use of it is not separately
observable, and no test pretends otherwise.

A job that died of a signal is reported (`sh: sleep: segmentation
fault`) rather than left to look like an exit status, except for
`SIGINT` -- the terminal has already echoed `^C` -- and `SIGPIPE`, which
is how the second half of a pipeline tells the first to stop.

### Jobs proper

`&` puts a pipeline in the background; `jobs`, `fg` and `bg` manage what
is running. A job is remembered by its process group and its stages:

- A job takes a **number only when it outlives the foreground** -- when
  it is backgrounded, or when it stops. Numbering every pipeline would
  print `[9]+ Stopped` on the ninth command of the session, which is not
  what a job number means.
- `^Z` stops the foreground job; the shell waits for **every** live
  stage to report stopped before it prints `[1]+  Stopped` and takes the
  terminal back. Returning on the first would hand the terminal to the
  shell while the other stages were still on their way to parking, and
  they would write over the prompt. `fg` hands the terminal over, sends
  `SIGCONT` to the *group* so every stage resumes together, and waits
  again; `bg` continues it without the terminal.
- A stopped foreground job is not reported as a death: `job_wait_foreground`
  answers `JOB_STOPPED` (128 + `SIGTSTP`, which is what `$?` is after a
  `^Z`) and the caller tests for it by name before calling
  `report_signal`.
- Background jobs are collected before each prompt with
  `waitpid(-1, WNOHANG | WUNTRACED | WCONTINUED)`, which is where a
  shell reports `Done` and `Stopped`.
- The shell ignores `SIGTTOU`, `SIGTTIN` and `SIGTSTP` as well as
  `SIGINT` and `SIGQUIT`. `SIGTTOU` is the load-bearing one: taking the
  terminal back after a job is done from the background, so a shell that
  did not ignore it would stop itself every time a job finished.

Without job control -- a non-interactive shell, or one that could not
claim a terminal -- `&` says so and runs the pipeline in the foreground
rather than pretending to background it.

### Line editing

An interactive shell reads its line in **raw mode** and draws it itself,
which is the only way to have arrow keys or history: in canonical mode
the kernel owns the line and hands it over only when it is finished.
Left and right arrows move the cursor, up and down walk a
thirty-two-entry history, `^W` erases a word, `^U` the line, `^A`/`^E`
jump to its ends, and `^C` abandons it and prompts again.

Three details are load-bearing:

- **An escape sequence is consumed whole, and an `Escape` that is not
  the start of one gives its next byte back.** A CSI sequence is
  parameter bytes, then intermediates, then one final byte in
  `0x40`-`0x7e`. Reading a single byte after `[` was right only for the
  four arrow keys and left the tail of everything else on the line, so
  Delete (`Esc [ 3 ~`) typed a `~`; the shell now reads to the final
  byte and ignores what it does not know. A byte that no CSI sequence
  can contain -- a control byte, Enter among them -- ends the sequence
  and is given back, so an unfinished `Esc [` cannot swallow the Enter
  that would have submitted the line. A byte after `Escape` that is
  not `[` is not part of a sequence at all, so it is handled as an
  ordinary keystroke -- `Escape` then `x` leaves an `x` on the line,
  where the first version silently ate it. The reads inside a sequence
  do block: with no `VTIME` there is no way to wait a moment for the
  rest and give up, so `Escape` alone waits for the next key. That is
  recorded as part of the `VTIME` gap rather than solved.
- **Typing at the end of the line echoes one character; only an edit
  that moves text about redraws.** A redraw is a carriage return, the
  prompt and the line, so redrawing on every keystroke would print a
  prompt per character -- and anything reading the serial log, the boot
  harness included, counts prompts to know when the shell is ready.
- **It falls back to a plain `read`** when the terminal cannot be put
  into raw mode: a script, a pipe, or a shell without job control. A
  shell that required a terminal would not be able to run `/etc/rc`.

The modes are restored before every command runs, so a job inherits a
cooked terminal and `^C` reaches it through the kernel as before. What
the shell does not know is the terminal's width, so a line longer than
the screen wraps in the terminal's own way and the redraw does not
account for it -- recorded rather than solved, because solving it means
tracking the cursor across rows.

### Input

Interactive: write the prompt to handle 2, `read(0, line, 1023)` (the
tty delivers one line, at most 1024 bytes per call; a longer typed line
arrives in pieces that run as separate lines, a recorded limit), strip
the newline; EOF (0 bytes) prints a newline and exits with the last
status. Script: `fgets` on the file. `-c`: the string is the whole
input.

### Errors

Syntax errors print `sh: syntax error near 'x'` and set `$? = 2`. A
command that cannot be spawned: 127 (not found) or 126 (not
executable). `-e` exits on any non-zero status outside `&&`/`||`
contexts.

## Utilities

Every utility follows the same shape: parse options by hand (a
`while (argc > 1 && argv[1][0] == '-')` loop; no `getopt` yet), do the
work with libc, `perror`-style messages `name: path: strerror`, exit 0
on success and 1 if anything failed. Buffers are 16 KiB stack arrays
for copies. `ls` sorts names with `qsort`. `cp -r`/`rm -r` recurse with
`opendir` (depth bounded by `PATH_MAX`). `mv` tries `rename`, falls
back to copy plus unlink on `EXDEV`. `ps` reads `procinfo` twice if the
first buffer was too small. `kill` accepts `-9`, `-KILL`, `-s KILL`.
`dmesg` writes the `klog` buffer. `sysctl -a` lists `sysctl.names` then
each value as `name = value`.

## ssh (roadmap M4)

The installed system's ssh server is dropbear (`ports/dropbear/README.md`),
a service `cosmo-install` writes (`/etc/svc/dropbear`): its host key is made
at the system's first boot and kept; logins are root's, by public key alone
(`/root/.ssh/authorized_keys`), into BusyBox ash -- on a pseudo-terminal
with job control when the client asks for one. The live image runs no ssh
server.

## netcfg (`userland/system/netcfg.c`; roadmap M4)

DNS for user space without a resolver in the kernel. The kernel's DHCP
client holds each interface's lease (docs/kernel-services/network/design.md,
"The DHCP client"); `/dev/net/tapctl`'s snapshot lists the leases in its
lease section (netctl version 7). `/etc/rc` runs `netcfg` after the
services start: it waits up to 10 s (`--wait`) for a bound lease, prints
each bound one, and writes one `nameserver` line per distinct DNS server
the leases name to `/etc/resolv.conf` (`--resolv`), through a temporary
file renamed over it so a reader never sees half a file. musl's resolver,
which BusyBox's `wget` reaches through `getaddrinfo`, reads that file;
BusyBox's `nslookup` reads it too. No lease with a DNS server (an
interface configured from fw_cfg, or no DHCP server) leaves the file as
it was and exits 1. `netcfg show` prints the leases: state, address and
prefix, gateway, server, lease, T1/T2 and the time left, DNS servers,
and the host name a lease carries.
The file is written once per boot: a renewal that changed the servers is
picked up at the next boot (or another `netcfg`).

**The host name** (shell completeness S1). A lease whose server named the
machine (DHCP option 12, netctl version 8; the kernel keeps only a name of
letters, digits, `-` and `.`) names it here too: `netcfg` writes the first
held lease's name to `/etc/hostname` (`--hostname`), the same
write-and-rename way, and sets it with `sethostname`. `/etc/rc` applies
`/etc/hostname` with BusyBox's `hostname -F` before `netcfg` runs, so a
name a server gave once is kept by a later boot whose server names none,
and a name written there by hand is the machine's until a server names
another.

## syslogd (`userland/system/syslogd.c`; shell completeness S2)

`/dev/log` into the kernel's log. musl's `syslog(3)` -- BusyBox's
`logger`, and any program that logs -- sends each message as a datagram to
the unix socket `/dev/log` as `<PRI>Mmm dd hh:mm:ss TAG: TEXT`. The
`syslogd` service (`/etc/svc/syslogd`, `privileges none`, `restart
always`) binds it and writes each message with `SYS_log` as `syslog:
[FACILITY.LEVEL] TAG: TEXT`: the priority by name, the timestamp dropped
(the kernel's line has its own), control characters blanked, the line cut
to what one log line takes (199 bytes). `dmesg` and the console show it;
nothing is written to a file. At its start it clears the path only of
a socket node no one is bound to (an earlier run's: a probe `connect`
fails with `ECONNREFUSED`, the one error that says so): a file there, a
socket another `syslogd` serves, or one the probe cannot judge (any other
error) is refused and left alone. Before that check it binds the
abstract unix name `syslogd:`, the inode of the directory holding the
socket and its last component -- the endpoint's identity, so every
spelling of the path is one name -- held for its whole life: a second
start fails there (`EADDRINUSE`), so two starts cannot both judge one node
stale and the second unlink what the first has just bound, and the name
goes with the process however it ends. (stat gives no device number, so
two directories with one inode number on different filesystems share a
name: a start can be refused that need not be, never the reverse.) With no
privileges the kernel's rate limit
for an unprivileged writer applies to it as to any process: a burst past
it is dropped, and the next line that gets through says how many were.
The socket's node is the kernel's 0755, owned by root (the native ABI has
no `chmod`), so only root's processes can send to it; every login here
is root's (inventory).

## ip (`userland/system/ip.c`; shell completeness S1)

`/sbin/ip` shows the machine's addresses, links, IPv4 routes and ARP
neighbours in iproute2's format: `ip a`, `ip addr`, `ip link`, `ip r`,
`ip route`, `ip neigh`, each with `show [dev] NAME` and `-4`/`-6`,
abbreviated as iproute2 allows. BusyBox's `ip` speaks only netlink, which
the kernel does not have; this one reads `/dev/net/tapctl`'s snapshot
(netctl version 8: the interface, route and neighbour sections, and the
lease section for which addresses are DHCP's, shown `dynamic` with the
lease's time left and `proto dhcp` -- while the interface still has the
lease's address and mask, and the default route the lease's router; an
address set by hand is shown static). It is the deliberate substitute until
netlink exists, and read-only: any command that would change something
(`add`, `del`, `set`, `flush`) is refused with exit 2, and BusyBox's
`ifconfig` and `route` configure (under `PRIV_NET`). Its lines follow
iproute2's field for field and leave out what the kernel does not report
(qdisc, group, qlen, a link mode), as iproute2 does when a kernel omits an
attribute; IPv6 shows each link-local address, the one link-local route
there is (through the default interface, which `ipv6_route` sends every
link-local destination to) and no neighbours. Reading the snapshot takes `PRIV_NET`, as every netctl read
does, so an unprivileged `ip` is refused where Linux's would answer
(inventory).

## Mounting (shell completeness S6)

`mount` and `umount` are BusyBox's, over the Linux personality's
`mount(2)` and `umount2(2)` (docs/compat/linux/design.md, "Mounting"):
`mount -t cosmofs /dev/vdb /data`, `mount -t ramfs none DIR`, `mount -t
proc proc DIR`, `-o ro`, `umount [-f] DIR`, and `mount` alone listing
`/proc/mounts`. Each needs `PRIV_MOUNT`, as the native calls do. A device
is `/dev/NAME`, the name `lsblk` shows and `/proc/mounts` prints, though
`/dev` holds no block device nodes: the door reads the name. BusyBox's
`LABEL=` and `UUID=` sources need its `volume_id`, which knows no cosmofs
(off: `FEATURE_MOUNT_LABEL`); so do loop devices, NFS and CIFS, and an
`/etc/mtab` (the mount table is `/proc/mounts`).

**`/etc/fstab`** is read by `mount -a`, which `/etc/rc` runs before the
services start; the image's lists nothing (the root is init's, from the
command line), and a line a user adds -- `/dev/vdb /data cosmofs defaults
0 0` -- is mounted at every boot after. A line that fails is reported on
the console and the rest still mount. The installer copies `/etc`, so an
installed system starts with the same empty table.

**`lsblk`** (`userland/system/lsblk.c`) prints util-linux's default
listing from `/dev/blkctl`'s device list and `/proc/mounts`: each disk,
its partitions under it in table order (`|-`, and `` `- `` for the last),
size in powers of 1024, read-only, type and mount point (a second mount
point of the same device on a line of its own). It leaves out
`MAJ:MIN` (no device numbers here) and `RM`. BusyBox has no `lsblk`.

**`blkid`** (`userland/system/blkid.c`) prints util-linux's line per
device: `/dev/vda2: LABEL="cosmo-root" UUID="..." TYPE="cosmofs"
PARTUUID="..."`. It reads the device's first block through `/dev/blkctl`
-- of a cosmofs's two superblock slots the valid one (magic, version,
CRC32C) with the higher generation, as a mount chooses, for its label and
pool uuid; or a disk's GPT, accepted by the kernel's own `gpt_parse`
(`PTUUID`, `PTTYPE="gpt"`) -- and takes a partition's `PARTUUID` from the
kernel's partition list. A partition holding anything else shows its
`PARTUUID` alone; the ESP's FAT is not identified. BusyBox's `blkid`
knows no cosmofs and prints no `PARTUUID`, and opens `/dev` nodes this
kernel does not make.

**`mkfs.cosmofs [-L LABEL] DEVICE`** (`userland/system/mkfs.cosmofs.c`)
makes an empty single-device cosmofs with `/dev/blkctl`'s `FORMAT`, which
carries the label (up to 16 printable characters, no blank, quote or
backslash). A whole disk with registered partitions is refused, by the
tool and by `/dev/blkctl`: the partitions would stay over the new
filesystem, and the backup table would bring them back at the next boot. The installer names the root it makes `cosmo-root` the same
way. The three tools need `/dev/blkctl`, which is root's.

## Test script (`userland/etc/rc.test`)

Straight-line shell (no control flow exists): every check is a command
whose failure is detected with `||`:

```sh
echo hello | cat > /tmp/t1 || echo "SHTEST: FAIL pipe"
cat /tmp/t1 | cat | cat > /tmp/t2 || echo "SHTEST: FAIL pipe3"
...
false; test "$?" ...   (no `test`: use the utilities' own exit codes and `&&`/`||`)
echo "SHTEST: PASS"
```

Two sections depend on fixtures only the x86-64 build carries: the Linux
programs run through `sh /etc/rc.linux` only if `/boot/tests/linux/lxhello`
exists, else the script prints `LINUXTEST: skipped`; `vmctl probe` fails
without `/dev/vmm` and the script prints `HVTEST: skipped`. The harness
requires the `skipped` lines on AArch64 and forbids them on x86-64.

Since there is no `test` utility, checks compare through behaviour:
commands that must succeed carry `|| FAILS=1`, commands that must fail
carry `&& FAILS=1` (`rm` of a file that must not exist, `ls` of a
removed directory, `cat` of a missing file, `kill` of a bad pid), and
outputs are re-read with `cat` into the log where a person can see
them. The last line, `sh -c "exit $FAILS" && echo "SHTEST: PASS" ||
echo "SHTEST: FAIL $FAILS"`, prints the verdict; the harness requires
`SHTEST: PASS` in self-test builds. The exact sequence is in
`testing.md`.

## Interactive harness (`tests/boot/shelltest.py`)

Started by `run_boot_test.py` for normal runs (never for the panic
run): QEMU's stdin is a pipe; a thread follows the serial log; when it
holds one more `cosmo$ ` prompt than commands sent it writes the next
command from `COMMANDS` (`echo interactive-ok`, `ls /bin`, `ps`, an odd
`echo $((`, `pwd`, `cd /tmp && pwd && cd /`, `sysctl kernel.name`,
`dmesg`, `nosuchprogram`, `exit 0`) with a `\n`; the run must then end
through init's exit. Each command has patterns the log must contain
(`testing.md`); `run_boot_test.py` also requires `^interactive-ok$`,
`^init: shell exited with status 0`, `^init: CosmoOS userland, pid \d+`
and, in self-test builds, `^SHTEST: PASS`.

## Services (`userland/system/svc.c`)

Constitution section 55 asks for start, stop, restart, dependencies,
logging, a restart policy, resource limits and supervision, and says not
to reproduce systemd. What it describes is daemontools' shape, and that
is what this is.

**There is no service daemon.** One supervisor process per service, and
the state is in the filesystem. A central manager would need a control
channel, and when `svc` was built the two Unix answers to that -- a
named pipe and a unix socket -- were both things this kernel did not
have (`mknod` was not in `vnode_ops`, and sockets were AF_INET). The
unix-sockets unit has since built the second and the named-pipes unit
the first (`mkfifo`, and a FIFO in the shell test); `svc`'s shape
stands on its own reasons. Inventing a third would mean
building an IPC mechanism in order to build a service manager, which is
backwards. It also means a supervisor that dies takes one service with
it rather than all of them, and that `svc` is an ordinary program with
no privileged position: what it knows, `cat` can read.

```text
  /etc/svc/<name>        the service: key value lines, one per line
  /run/svc/<name>.pid    the supervisor's pid, once the service has run
  /var/log/svc/<name>    the service's output and its supervisor's notes
```

A definition is `key value`, no sections and no expressions:

```text
  exec /sbin/thing -f        what to run (required)
  after net time             start these first
  restart on-failure         never (default) | on-failure | always
  retries 5                  give up after this many restarts (default 5)
  backoff-ms 100             wait this long, doubled each retry, capped at 5s
  user 1000                  and group, via COSMO_SPAWN_SETCRED
  root /srv/thing            confine it there (COSMO_SPAWN_SETROOT)
  mountns yes                a mount namespace of its own
  utsns yes                  a uts namespace of its own
  domain yes                 a process domain of its own
  limit-nofile 32            any COSMO_RLIMIT_* by name
  privileges net cred        keep exactly these privileges, drop the rest (M5)
```

`privileges` (roadmap M5) names the privileges the service keeps --
`mount`, `sys_admin`, `net`, `proc`, `cred`, `device`, or `none` -- and
svc spawns it with `COSMO_SPAWN_DROPPRIV` for the others; a name it does
not know fails the service. Without the line the service has the
supervisor's privileges. The installed system's `dropbear` service says
`privileges net cred`: port 22 and a session's `setgid`/`initgroups`/
`setuid`, nothing more (`docs/kernel/security/design.md` §1).

## priv (`userland/system/priv.c`, roadmap M5)

`priv` prints the calling process's privileges (the `privs:` line of
`/proc/self/status`). `priv [-d LIST | -k LIST] [-u UID [-g GID]] [--]
COMMAND [ARG...]` runs COMMAND without the privileges in LIST (`-d`), or
with only those (`-k`), optionally as UID/GID (`COSMO_SPAWN_SETCRED`,
which drops every privilege when no id stays 0), waits, and exits with
its status: 125 when priv itself fails, 127 when COMMAND cannot start.
LIST is comma-separated names or `all`. A child never holds what priv
lacks, so `priv -k all` inside a dropped process gives nothing back.

**A definition that is not understood fails the service**, and that
covers more than unknown keys. A typo in `root` or `user` would
otherwise leave a service running with more authority than its author
wrote down, and the file is the only place that authority is stated.
So: an unknown key is an error; a number that is not a number is an
error -- `user daemon` through `atoi` is uid 0, which makes a typo in
the one key that reduces privilege grant the most of all; a limit that
cannot be set stops the service rather than running it unrestricted;
and an unreadable definition is fatal to that service rather than a
default.

The same rule applies to reporting. `svc start` says a service started
only when it has seen it running or seen its supervisor finish having
run it -- reporting success without looking would make `svc boot` start
the dependents of a service that never ran. And more definitions than
`svc` can hold is a message naming the first one left out, not silence,
because which ones were dropped would otherwise depend on the order the
directory happened to be read in.

This is where the container primitives earn their place: a service is
confined by naming it in a file, and `svc` does no more than turn those
lines into the flags that already exist (`docs/kernel/security/design.md`
sections 1--1f).

### Supervision

`svc --supervise <name>` is the supervisor: it spawns the service, waits
for it, and decides. It writes its pid file only after the first spawn
succeeds, because that file is what `svc start` reads as "the service is
up" -- written earlier, a spawn that then failed would look like a
running service for as long as it took to fail. `svc start` spawns that and returns, so the
supervisor is reparented to init, which reaps it -- there is no fork
here, and none is needed.

The restart policy is bounded in both directions. `never` and a clean
exit under `on-failure` mean the supervisor exits with the service.
Otherwise it waits `backoff-ms`, doubles it each time up to five
seconds, and gives up after `retries` restarts, writing why. **A service
that dies instantly must not spin the machine**, which is what a
supervisor without a backoff and a limit does; and a supervisor that
gives up must say so where somebody will find it, which is the log.

`svc stop` is two kills, and their order matters. The supervisor goes
first, or it would see its service die and start another one -- killing
the service alone is a restart, not a stop. Then the service itself,
whose pid the supervisor records in `/run/svc/<name>.child` for exactly
this purpose.

The supervisor does not do the second kill, and that used to be a fact
about this kernel rather than a choice: `svc` installs no signal
handler, so being told to stop kills it where it stands with no chance
to tidy up. Since the signals unit a native process *can* catch
`SIGTERM`; teaching the supervisor to is a change to `svc`, which
nothing has asked for yet.
`svc` therefore removes the pid files as well. The first version of this
did signal only the supervisor, and a `sleep 30` service outlived its
own stop by half a minute.

### Order

`svc boot` reads every definition, sorts by `after`, and starts in that
order. A cycle is refused and named. A service whose dependency did not
start is not started either, and says which one -- a dependency that is
ignored when it fails is a dependency in name only.

Order here is *start order*, not readiness: `svc` knows a service is
running, not that it is ready to serve. Readiness needs the service to
say so, which needs a channel, which is the thing this design does
without. Where that matters the dependent must retry, which it must do
anyway on a machine where anything can restart.

Not done, and each for a reason: socket activation (there are no unix
sockets to activate on), a syscall filter per service (it would need a
name-to-number table in userland that nothing else wants yet), timers,
and any readiness protocol.

## The installer (`userland/system/cosmo-install.c`)

Roadmap M2. Non-interactive; everything through `/dev/blkctl`
(`docs/kernel-services/vfs/design.md`, "The block-device channel").

1. List the devices. The source ESP is the partition flagged `BOOT` (the
   one the loader was read from); without one there is nothing to copy
   (exit 3). The target must be a whole disk, writable, not mounted, not
   the boot disk, 512- or 4096-byte sectors, with room for the ESP and a
   32 MiB root; one that already has a GPT header (either copy) needs
   `--force`.
2. Lay out the table with `gpt_build` (the kernel's own file, built for
   user mode): partition 1 the ESP type at 1 MiB, exactly the boot ESP's
   size; partition 2 the CosmoOS root type from the next MiB to the last
   usable sector. GUIDs are version 4 from SHA-512 over the 16
   `COSMO_AT_RANDOM` bytes and a counter. The table is parsed with
   `gpt_parse` from memory before anything is written.
3. Write the backup copy, then the primary (until the primary lands the
   disk has no table the kernel accepts), flush, `RESCAN`: the two
   partitions appear.
4. Copy the boot ESP to partition 1 sector for sector, unchanged. Its
   command line is signed with the kernel (verified boot, roadmap M5)
   and says `root=boot-disk`: the CosmoOS root partition on the disk the
   loader was read from, which is partition 2 once the installed disk
   boots (docs/boot/design.md, "Verified boot"). The installer writes
   nothing on the ESP and signs nothing, and so refuses (exit 3) an image
   whose signed command line says anything else (`kernel.root` is not
   `boot-disk`): that disk would not boot its own root.
5. `FORMAT` partition 2 as cosmofs named `cosmo-root` (what `blkid`
   prints; shell completeness S6), mount it at `/mnt/cosmo-install`,
   create `/dev`, `/proc`, `/tmp` (01777), `/mnt`, `/var`, `/var/log`,
   `/var/lib/pkg`, `/boot`, and copy `/bin`, `/sbin`, `/etc` and, when
   present, `/usr` and `/var/db` (files with their modes, directories,
   symbolic links; device nodes, FIFOs and sockets are not copied).
   `/boot` stays empty: the boot archive is on the ESP. Since roadmap M6
   the image's package repository (`/boot/repo`, the boot archive's
   `repo/`) is copied to `/var/lib/pkg/repo` and the installed
   `/etc/pkg/repos.conf` names that directory, so `pkg` works on the
   installed system; the keys that verify it (`/etc/pkg/keys`) came with
   `/etc`.
6. `sync`, unmount, flush the disk, print the result.

Any failure after the table was written unmounts what it mounted and
wipes both copies of the table, so a failed install never leaves a disk
that looks bootable. The installer writes no FAT: the ESP is the boot
ESP's bytes.

## Updating (`userland/system/cosmo-upgrade.c`; shell completeness S7)

`cosmo-upgrade` moves an installed system to another release without
reinstalling it, and back.

    cosmo-upgrade [--check] [--stop-after=STEP] DIR
    cosmo-upgrade [--check] [--stop-after=STEP] --fetch TAG|URL
    cosmo-upgrade --check
    cosmo-upgrade --rollback
    cosmo-upgrade --status

DIR holds a release's boot set: `kernel.elf`, `boot.tar`, `cmdline` and
`manifest`, under those names or as the release assets name them
(`cosmoos-ARCH-kernel.elf`, ...), with the release's `SHA256SUMS` and
`SHA256SUMS.sig`. How it got there is the operator's
business: `wget`, `scp`, a mounted disk (S6), or `--fetch` (below). On v1.0.3 the files belong in `/tmp`
(ramfs): v1.0.3's cosmofs can lose a large file being written (the record
fallback, fixed since; `docs/kernel-services/filesystem/cosmofs/design.md`). The release's `cosmo-upgrade` runs the
upgrade: a v1.0.3 system has none, so the first upgrade is started with
the release's binary. It comes in `cosmoos-ARCH-cosmo-upgrade.cpk`, a
package of `sbin/cosmo-upgrade` and `sbin/cosmo-verify` signed with the
release key (`scripts/mkrelease.py`, `pkgbuild.py pack`), which v1.0.3's
`pkg install -n` checks against the key v1.0.3 shipped in
`/etc/pkg/keys`, writing nothing; BusyBox `tar` then unpacks the checked
file into `/tmp`. Nothing older than the package can check an Ed25519
signature on v1.0.3: its `pkg` is the only verifier it has. From v1.0.6
the package also carries `bin/cosmo-fetch`, for `--fetch` on a system
older than it (`COSMO_FETCH` names the copy; `userland/lib/fetch.c`).

### Fetching (roadmap 1.1 P6)

`/etc/cosmo-release.conf` names the release index, `url
https://vkuttyp.github.io/CosmoOS/releases` (the default when the file is
absent), and may add `pin sha256:HEX`. The release workflow publishes each
release's set there (`.github/workflows/repository.yml`,
`scripts/publish-repo.py`): `TAG/SHA256SUMS`, `TAG/SHA256SUMS.sig` and the
files under their release names, and `LATEST` (`tag: TAG`, `published:
T`). Everything is fetched through `/bin/cosmo-fetch` (the certificate
checked against the CA bundle, same-origin redirects only):

- `--fetch TAG` (or a URL of such a directory) fetches into
  `/var/cache/cosmo-upgrade/TAG/`: `SHA256SUMS` and its signature first,
  verified against the ring before anything else is fetched (a refusal
  is exit 3, `nothing more fetched, nothing written`); then the four set
  files, each resuming a cut transfer and checked against `SHA256SUMS`
  (fetched once more from the start if a kept part does not match); then
  the upgrade proceeds as for `DIR`, every check made again. A completed
  upgrade removes the fetched files; `--check --fetch` keeps them.
- `--check` alone fetches `LATEST` and compares its tag with the running
  kernel's build id (the last word of `/proc/version`, `git describe`'s
  `vX.Y.Z[-rcN][-N-gHASH]`): `TAG is newer than this system's ID ...:
  cosmo-upgrade --fetch TAG`, or `this system's ID is up to date: ...`.
  Both exit 0; a build id that is not a release's is said so.

The index is GitHub Pages rather than GitHub's release assets or its API:
an asset's URL redirects to another host, which `cosmo-fetch` refuses,
and the API is rate-limited without a token. Pages serves the files at
one stable HTTPS URL, and its publication needs no secret but the release
key.

### Trust

Before anything of the release is used, its `SHA256SUMS` is checked
against `SHA256SUMS.sig` and the running system's key ring,
`/etc/pkg/keys`, the same trust root as `pkg` (PK9; `relsig.c`). The
signature is the `COSMOSIG` trailer of `scripts/modsign.py` on its own (88
bytes: signature, key id, version, algorithm, magic) over the file's
bytes; `make release` signs each architecture's `SHA256SUMS` with the
build's key, and the release workflow signs the published one, which
joins both, with the release key. The four files are then checked against
the lines `SHA256SUMS` gives `cosmoos-ARCH-<name>`; a missing signature,
a key outside the ring, a bad signature and a file that does not match
are each refused, exit 3. `cosmo-verify FILE SUMS SIG` is the same check
for one file, by its base name. Then the manifest is checked against the
ring: its size, the `COSMOSIG` trailer, a key in the ring, the Ed25519
signature, the magic, version and file names, and each file's size and
SHA-512. The loader is not in the manifest. It travels inside `boot.tar`
(`efi/BOOTX64.EFI` or `efi/BOOTAA64.EFI`, since S7), so the signature
covers it too. A release whose archive carries no loader is refused. Any
refusal exits 3 and leaves the ESP and the root untouched. To trust another
key, the operator puts its `.pub` file in `/etc/pkg/keys`. The upgrade
adds the new release's own key (its `etc/pkg/keys/`) and removes none, so
a rollback still verifies against the keys that signed the previous set.

The loader is a second, separate check. At boot the new loader verifies
the new set against the keys compiled into it, and it boots nothing else.
So before writing, `cosmo-upgrade` also checks that the new loader trusts
the key that signed its own set and the key that signed the current set,
which it starts between the two switches; `--rollback` checks that the
loader the firmware starts trusts the key that signed `\cosmo\prev`. The
loader's ring is a table of `{ id, pub, name }` (`kernel/keyring.h`) with
revoked keys refused at build time, so an 8-byte id in the loader image
followed by a public key that verifies the set's manifest is that key in
its ring. A loader that fails either check is refused, exit 3.

One run at a time: `cosmo-upgrade` holds an abstract unix socket name
for its life (as syslogd does), and a second run is refused, exit 3. Two
runs would each plan the FAT from their own copy and hand out the same
free clusters. The kernel drops the name however the process ends, so a
killed run leaves no lock behind.

### The ESP: FAT32 through `/dev/blkctl`

Nothing on an installed system mounts FAT. `cosmo-upgrade` carries a
small FAT32 reader and writer (`upgrade_fat.c`) over the raw sectors of
the partition flagged `BOOT` (the one the loader was read from). It
refuses anything but FAT32 whose sector is the device's sector, and it
updates both FAT copies (or the active one when mirroring is off) and the
FSInfo free count. The ESP is not mounted (V38), so `blkctl` writes to
it while the root, a sibling partition, is mounted.

The loader reads fixed paths, `\cosmo\{kernel.elf,boot.tar,cmdline,
manifest}` and `\EFI\BOOT\BOOTX64.EFI`, and it has no fallback. So the
replacement is a switch, never an overwrite. The layout it keeps:
`\cosmo` holds the set, the loader that set came with
(`\cosmo\BOOTX64.EFI`) and `prev`; `\cosmo\prev` holds the previous set
and its loader; `\EFI\BOOT\BOOTX64.EFI`, the one the firmware starts, is a
copy of one of them in clusters of its own. A set's loader moves with the
set, in the same sector write, so no interruption can lose it. (The first
version kept only `\EFI\BOOT`'s copy: an upgrade stopped after its loader
switch and run again swept v1.0.3's loader away, and `\cosmo\prev` then
held the new one. The acceptance test found it.)

1. **Sweep.** Clusters marked allocated but reachable from no directory
   (left by an interrupted run) are freed. Entries holding a reserved or
   bad-cluster value (`0x0FFFFFF0` to `0x0FFFFFF7`) are left alone.
2. **The set's own loader.** On an ESP no upgrade has touched, `\cosmo`
   has none: a copy of `\EFI\BOOT`'s is written and its entry added to
   `\cosmo`, one sector write. The loader ignores the extra file.
3. **Drop the old `\cosmo\prev`.** Its entry is marked deleted, one sector
   write, and the sweep frees its clusters. Three sets need not fit.
4. **Write.** The new kernel, archive, command line and manifest, the
   new loader twice (the set's copy and `\EFI\BOOT`'s), and a new `prev`
   directory cluster whose entries name the current set and its loader:
   all in free clusters, then the FAT chains (both copies), then FLUSH.
   Nothing reachable has changed yet.
5. **Switch the loader.** `\EFI\BOOT`'s 32-byte entry is rewritten in
   place to name its new copy, one sector write, then FLUSH. The loader
   comes first because the new loader boots the old set (the boot code is
   unchanged since v1.0.3, and the check under "Trust" has found the old
   set's key in its ring), while the old loader cannot verify a set signed
   by a key it lacks.
6. **Switch the set.** A pending marker, `/var/lib/cosmo-upgrade/pending`,
   is written on the root naming the new set (its manifest's SHA-512).
   Then one sector write to `\cosmo` changes its four file entries and
   its loader's entry to the new clusters and adds `prev`, naming the new
   `prev` directory. Then FLUSH. This is the commit point.
7. **Sweep** again (the old `\EFI\BOOT` copy goes, a copy of it lives in
   `prev`), FSInfo gets the free count, the root pass runs (below), and
   the marker is removed.

An interruption anywhere leaves the old set, the new loader with the old
set, or the new set. Each of them boots verified. The argument rests on
a one-sector write being atomic, as the interfaces this system drives
provide (virtio-blk, NVMe, AHCI write a sector whole). `\cosmo`'s entries
must therefore share one sector, as `mkimage.sh`'s layout gives (8.3
names with the lowercase flags, no long-name entries, one cluster; eight
entries of the sector's sixteen). An ESP laid out otherwise, or with
unknown entries in `\cosmo`, is refused before writing.
`--stop-after=written|loader|set` ends the run after that step, exit 4.
It is how the acceptance test interrupts an upgrade (below).

A run that finds the marker naming the current set finishes the run that
wrote it instead of starting another: it makes `\EFI\BOOT`'s loader the
set's own if it is not, runs the root pass from `\cosmo\prev` to the
current set, and removes the marker. A rerun with the same DIR after the
set switch does the same, since the set is already current. Without the
marker a rerun of `--rollback` interrupted between its two switches would
go forward again.

`--rollback` is the same switch in the other direction. It verifies
`\cosmo\prev` against the ring, writes a new `prev` directory naming the
current set and its loader, writes the marker, switches `\cosmo` to the
previous set and its loader, and then finishes. The set goes first when
the loader the firmware starts trusts the previous set's key (rolling
back from a newer build to v1.0.3). Otherwise, when `\cosmo\prev`'s own
loader trusts the current set's key (rolling forward again from v1.0.3,
whose loader lacks the newer key), a fresh copy of that loader becomes
`\EFI\BOOT`'s first, as in an upgrade. When neither holds the rollback is
refused, exit 3. Finishing is a fresh
copy of the previous loader for `\EFI\BOOT` (when it is not that loader
already), the sweep, the root pass. A
second rollback goes forward again. A rollback to a release without
`cosmo-upgrade` (v1.0.3) removes `/sbin/cosmo-upgrade` with the rest of
the newer base system, so the second one is run with the newer release's
binary, fetched as for the first upgrade. If `\cosmo\prev` cannot be read while a
root pass is pending, the run fails and the marker stays.

### The root: the base system

The installed root holds a copy of the boot archive's `bin/`, `sbin/`,
`etc/` and `usr/` (the installer copied the live system, which the kernel
populated from the archive, `ramfs_populate_boot`), and its `repo/` at
`/var/lib/pkg/repo`. After the set switch, `cosmo-upgrade` brings the root
from the old archive to the new one. The old archive is the
`\cosmo\prev\boot.tar` it just moved, checked against its manifest
first. The rules:

- `bin/`, `sbin/`, `usr/`: each file and link of the new archive is
  written to a temporary name beside its target, `TARGET.cosmo-upgrade`,
  and renamed over it. The name is the tool's: one an interrupted run
  left is replaced by the next. A
  file keeps its existing mode; a new one gets the mode the kernel gives
  that archive path (0755 under `bin/`, `sbin/`, `usr/bin/`, `usr/sbin/`,
  `usr/libexec/`, else 0644). A path the old archive had and the new one
  lacks is removed.
- `etc/`: configuration. A file still holding the old archive's bytes is
  replaced. A file the installer or the operator changed (for example
  `/etc/passwd`, `/etc/pkg/repos.conf`) is kept, and the new version is
  written beside it as `FILE.new` when it differs. `FILE.new` is the
  tool's too: an earlier run's is replaced by this release's version. A
  new file is added. Nothing in `/etc` is removed.
- `repo/`: `/var/lib/pkg/repo` becomes the new archive's repository, so
  `pkg update && pkg upgrade` then upgrades the installed packages from
  it. Their index is verified against the same ring.

Everything else on the root (`/root`, `/home`, `/var` apart from the
repository, the package database, files the operator made) is left
alone. The pass converges, so an upgrade interrupted between the set
switch and the end of the root pass is finished by running it again (the
marker, above). Meanwhile the system boots: the new kernel runs the
static binaries of either release.

`--rollback` runs the same pass from the current archive to the previous
one, so the base system and the repository return with the boot set.
Packages that `pkg upgrade` changed stay changed: a package upgrade
cannot be rolled back (`docs/pkg/invariants.md`). The sizes fit: a release
set is about 16 MB, the ESP 64 MiB.

## Security

Programs are uid 0 like everything else so far. The shell passes only
the handles a child needs. Paths from the command line go to the kernel
which validates them. Nothing in userland trusts input length: the
shell's line is bounded (1024, the tty's limit), words are bounded by
the line, the argument vector by `ARG_MAX`.

## Future extensibility

- Control flow, functions, globbing, command substitution in `sh`
  (the parser produces a tree already; `expand.c` gains word splitting
  and pathname expansion; `$(...)` needs a pipe and a spawn of `sh -c`).
- Services in init: a directory of service descriptions, dependency
  order, restart policy, a control socket; `init` becomes the
  supervisor and `getty`/login appear with users.
- `userland/networking/`: `ping`, `ifconfig`, `nc` on the libc socket
  API; `dhcp` when the kernel has UDP broadcast on eth0 configured.
- Ports (section 48) bring real coreutils; the hand-written ones remain
  the rescue set.
