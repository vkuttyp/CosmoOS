# scripts

Automation: toolchain setup, QEMU runners, CI helpers.

| Script | Purpose |
|---|---|
| `setup-dev-linux.sh`, `setup-dev-macos.sh` | Install the cross toolchain and test tools |
| `check-tools.sh` | Verify the toolchain before a build |
| `mkimage.sh` | FAT32 boot image: loader, kernel, boot archive |
| `mkbootarchive.py` | Reproducible ustar boot archive (`init`, modules, fixtures) |
| `modsign.py` | Ed25519 key generation, module signing, verification (pure Python) |
| `gen-keyring.py` | Generate the kernel's built-in key ring from `tools/keys/*.pub` |
| `check-module-elf.py` | Post-build checks on a signed module (ET_REL, W^X, metadata, trailer) |
| `check-kernel-elf.sh` | Post-link checks on the kernel (W^X segments, PT_NOTE) |
| `check-reproducible.sh` | Build twice, compare |
| `qemu-run.sh`, `find-firmware.sh` | Run under QEMU, locate OVMF |
| `gen-compile-commands.py` | `compile_commands.json` for editors |
