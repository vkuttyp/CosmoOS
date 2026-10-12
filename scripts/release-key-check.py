#!/usr/bin/env python3
"""release-key-check.py KEY PUB -- the seed in KEY derives the public key in
PUB; print the key id (roadmap M6). Exit 1 if not."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402

seed = modsign.read_hex_key(sys.argv[1], "release key")
pub = modsign.read_hex_key(sys.argv[2], "release public key")
if modsign.secret_to_public(seed) != pub:
    sys.exit(f"release-key-check: the key's public half is not {sys.argv[2]}")
print(f"release-key: key id {modsign.key_id(pub).hex()} ({os.path.basename(sys.argv[2])})")
