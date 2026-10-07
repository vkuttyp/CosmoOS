#!/usr/bin/env python3
"""Turn a boot's packet capture into fuzz_net_frame corpus files.

  QEMU_PCAP=/tmp/boot.pcap make ARCH=x86_64 BUILD=debug test
  python3 tools/pcap-to-seeds.py /tmp/boot.pcap tests/fuzz/corpus/net_frame

Reads a classic pcap (any snaplen, Ethernet link type), keeps the frames the
guest *received* -- addressed to its NIC's MAC or broadcast/multicast, not the
ones it sent (the stack drops its own frames as reflections) -- and writes
one file per distinct frame shape: the Ethernet type, the IP protocol, the
TCP flags and whether the frame has a payload. Within a shape the first
frame is kept, so the corpus stays small and stable across captures. Each
file is one fuzz_net_frame record: the control byte (fix checksums, and
retarget a TCP segment to the established connection), the length, the
frame; a TCP segment's addresses are stamped by the target at run time, so
the captured ones are left as they were.

Nothing from the capture is kept but the frame bytes. The guest's MAC is
QEMU's default for the user-mode NIC unless --mac says otherwise.
"""
import argparse
import hashlib
import os
import struct
import sys

CTL_FIX = 1 << 1
CTL_TO_CONN = 1 << 2


def frames(path):
    with open(path, 'rb') as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            raise SystemExit('not a pcap: short header')
        magic = struct.unpack('<I', hdr[:4])[0]
        if magic == 0xa1b2c3d4:
            end = '<'
        elif magic == 0xd4c3b2a1:
            end = '>'
        else:
            raise SystemExit('not a classic pcap (magic %08x); a pcapng needs converting first' % magic)
        linktype = struct.unpack(end + 'I', hdr[20:24])[0]
        if linktype != 1:
            raise SystemExit('link type %d is not Ethernet' % linktype)
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, caplen, origlen = struct.unpack(end + 'IIII', rec)
            data = f.read(caplen)
            if len(data) < caplen:
                return
            if caplen == origlen:
                yield data


def shape(frame):
    if len(frame) < 14:
        return ('short',)
    etype = struct.unpack('>H', frame[12:14])[0]
    if etype == 0x0806:
        op = struct.unpack('>H', frame[20:22])[0] if len(frame) >= 22 else 0
        return ('arp', 'request' if op == 1 else 'reply' if op == 2 else 'op%d' % op)
    if etype == 0x0800 and len(frame) >= 34:
        ihl = (frame[14] & 0xf) * 4
        proto = frame[23]
        l4 = 14 + ihl
        if proto == 6 and len(frame) >= l4 + 14:
            flags = frame[l4 + 13]
            doff = (frame[l4 + 12] >> 4) * 4
            names = ''.join(n for b, n in ((0x02, 'S'), (0x10, 'A'), (0x08, 'P'), (0x01, 'F'), (0x04, 'R')) if flags & b)
            return ('ip4', 'tcp', names or 'none', 'data' if len(frame) > l4 + doff else 'nodata',
                    'opts' if doff > 20 else 'noopts')
        if proto == 17 and len(frame) >= l4 + 8:
            dport = struct.unpack('>H', frame[l4 + 2:l4 + 4])[0]
            return ('ip4', 'udp', 'port%d' % dport if dport < 1024 else 'highport')
        if proto == 1 and len(frame) >= l4 + 2:
            return ('ip4', 'icmp', 'type%d' % frame[l4], 'code%d' % frame[l4 + 1])
        return ('ip4', 'proto%d' % proto)
    if etype == 0x86dd and len(frame) >= 54:
        nh = frame[20]
        if nh == 58 and len(frame) >= 56:
            return ('ip6', 'icmpv6', 'type%d' % frame[54])
        return ('ip6', 'nexthdr%d' % nh)
    return ('ether', '%04x' % etype)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pcap')
    ap.add_argument('outdir')
    ap.add_argument('--mac', default='52:54:00:12:34:56', help="the guest NIC's MAC (QEMU's default)")
    ap.add_argument('--max', type=int, default=48, help='at most this many files')
    args = ap.parse_args()
    mac = bytes(int(x, 16) for x in args.mac.split(':'))
    os.makedirs(args.outdir, exist_ok=True)
    seen = {}
    total = 0
    for frame in frames(args.pcap):
        total += 1
        dst = frame[:6]
        if dst != mac and not (dst[0] & 1):
            continue
        if frame[6:12] == mac:
            continue
        s = shape(frame)
        if s in seen:
            continue
        seen[s] = frame
    written = 0
    for s, frame in sorted(seen.items(), key=lambda kv: repr(kv[0]))[:args.max]:
        ctl = CTL_FIX
        if s[0] == 'ip4' and s[1] == 'tcp':
            ctl |= CTL_TO_CONN
        rec = bytes([ctl, len(frame) & 0xff, len(frame) >> 8]) + frame
        name = 'cap-' + '-'.join(str(x) for x in s) + '-' + hashlib.sha1(frame).hexdigest()[:8]
        with open(os.path.join(args.outdir, name), 'wb') as f:
            f.write(rec)
        written += 1
    print('%d frame(s) read, %d shape(s) kept, %d file(s) written to %s' % (total, len(seen), written, args.outdir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
