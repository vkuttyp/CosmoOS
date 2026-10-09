# Device lifecycle Unit 2: fault and interleaving sweep

## Scope and baseline

Started 2026-10-09 from main `2661385c14c388190d8becfe95388f80c23995d3`
(PR #337 merge, main CI run `37885005992` green on x86-64, AArch64 and litmus),
on branch `device-lifecycle-fault-sweep`. The targets are the Unit 1 audit's
NVMe late publication, AHCI probe rollback ordering, virtio-rng refill,
acknowledgement failures in NVMe/AHCI/xHCI/e1000e, and callback retirement
in xHCI and the network worker. Every finding will distinguish a proven
failure from an unreachable or unresolved suspicion.

## Evidence and dispositions

| Target | Verdict | Evidence | Action |
|---|---|---|---|
| NVMe submit across controller death | In progress | Synthetic debug interleaving planned between dead check and queue lock | Pending proof |
