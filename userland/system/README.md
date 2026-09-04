# userland/system

`/sbin`: `mount` `umount` `ps` `kill` `dmesg` `sysctl`
(docs/userland/api.md). `ps`, `dmesg` and `sysctl` use the native
`procinfo`, `klog` and `sysctl` system calls through `cosmo/*.h`.
`vmctl` arrives with the hypervisor.
