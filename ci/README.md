# ci/

The continuous-integration toolchain images (roadmap M6). `Dockerfile` has
two targets: `toolchain` (Debian trixie with clang, lld, LLVM, the
sanitizer runtimes, mtools, musl-tools, QEMU for both architectures with
their UEFI firmware, and the OpenSSH client), which every build, boot and
test job runs in, and `litmus` (Debian sid with herdtools7) for the
memory-order litmus job. Both bases are pinned by digest.

`scripts/ci-image.sh` builds and pushes them to
`ghcr.io/vkuttyp/cosmoos-ci` only when this `Dockerfile` changes (the tag
is the target name and the file's SHA-256 prefix) and hands the jobs
digest references. Each image lists its packages in
`/usr/share/cosmoos-ci/packages.txt`. Nothing here is used by a local
build; `scripts/setup-dev-linux.sh` installs the same packages on a
development host. See `docs/development.md`, "Continuous integration".
