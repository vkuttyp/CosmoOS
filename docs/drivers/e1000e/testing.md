# e1000e driver: testing

## What runs on every boot

With `QEMU_NIC=both` (the default), the boot test requires the interface
to register (`e1000e: pci:... is eth1`) alongside virtio-net's `eth0`,
and the `net-second-nic` self-test runs:

- it finds a second non-loopback interface, brings the default one down
  with `netif_set_up(eth0, false)`, and requires `netif_default()` to
  become the other;
- it resolves the gateway through it (`arp_resolve` against
  `10.0.2.2`, which the interface's own user-mode backend answers) and
  requires the reply to arrive — a frame out and a frame in through this
  driver's rings, with nothing else on the path;
- it brings `eth0` back up and requires it to be the default again.

That is the half of the two-interface question a driver can be checked
on: the registry, the lifetime rules and the data path. Two interfaces
carrying independent traffic at once needs an interface selector the
stack does not have (`docs/audit/next-subsystem.md`, "Tests").

## The shape that runs everything over it

`QEMU_NIC=e1000e gmake test` removes virtio-net, so this driver is
`eth0` and the default interface, and the entire `net-*` suite — ARP,
IPv4, IPv6, UDP, TCP, the harness echo against the host, reorder and
loss — runs through it. This is the check on Invariant 5: the suite was
written against no interface in particular, and this is the first time
that has been true in fact. It is a step in the verification chain.

## Confirmed against bugs

- With the receive handler's error check removed, a frame with the
  descriptor's error byte set would be handed up; the harness's TCP
  tests are the check, since QEMU's model does not generate errored
  frames on demand. Recorded as untested rather than claimed.
- With `RCTL.EN` left clear the device receives nothing, and
  `net-second-nic` fails at the ARP reply — the check that the test
  reaches this driver's receive path and not a cached answer.
- The first version wrote a "queue enable" bit into `RXDCTL`/`TXDCTL`
  and the design claimed QEMU required it. The same bug-reintroduction
  run showed it did not: with the bit clear every test passed. The
  writes are gone and the design says why; the claim had been made from
  memory of another Intel family, which is exactly what this step exists
  to catch.

## Not covered

Ring wrap under sustained load (the tests move a few hundred frames);
the watchdog firing (QEMU's model does not hang); link loss (QEMU's link
is always up); real hardware, per §61.
