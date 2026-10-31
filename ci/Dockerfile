# ci/Dockerfile -- the CI toolchain images (roadmap M6; docs/development.md,
# "Continuous integration"). Built once by the `image` job of
# .github/workflows/ci.yml when this file changes (scripts/ci-image.sh tags
# each target with this file's SHA-256) and pushed to
# ghcr.io/vkuttyp/cosmoos-ci; every other job runs in it by digest, so no job
# pulls from a public registry. The bases are pinned by digest: Docker's
# official Debian images through Amazon ECR Public's mirror.
#
# The package versions are whatever Debian served the day the image was
# built; the image digest, not this file, is what fixes them, and each image
# carries the list (/usr/share/cosmoos-ci/packages.txt) for the release notes.

# The build, boot and test toolchain: Debian trixie (QEMU >= 9.2 for the
# virtualization tests; docs/development.md). `sudo` because the user
# guide's commands use it and the guide job runs them as written; `gh` for
# the release workflow's publish step.
FROM public.ecr.aws/docker/library/debian:trixie@sha256:913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5 AS toolchain
LABEL org.opencontainers.image.source=https://github.com/vkuttyp/CosmoOS
LABEL org.opencontainers.image.description="CosmoOS CI toolchain (clang, lld, QEMU, UEFI firmware, ceesafec)"
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      git ca-certificates clang lld llvm libclang-rt-dev make mtools python3 musl-tools bzip2 xz-utils \
      qemu-system-x86 ovmf qemu-system-arm qemu-efi-aarch64 openssh-client sudo gh \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /usr/share/cosmoos-ci \
 && dpkg-query -W -f '${Package} ${Version}\n' > /usr/share/cosmoos-ci/packages.txt
# The pinned ceesafec (scripts/cee-gen.py's CEESAFEC_VERSION; docs/build/design.md, "CeeSafe sources"): every build runs it
# (build/cee.mk). Its Linux x64 release archive (github.com/vkuttyp/CeeSafe), checked against the release's SHA256SUMS;
# one native executable built from C, needing glibc 2.35 or newer, which the image has. A bump changes this file, so
# the image is rebuilt.
ARG CEESAFEC_VERSION=0.2.0
ARG CEESAFEC_SHA256=2e898a239f36fb7d0c39262d8ac59c98d8b98219e1fc4f0e4993136aec3e18cd
RUN python3 -c "import urllib.request; urllib.request.urlretrieve('https://github.com/vkuttyp/CeeSafe/releases/download/v${CEESAFEC_VERSION}/ceesafec-linux-x64.tar.gz', '/tmp/ceesafec.tar.gz')" \
 && echo "${CEESAFEC_SHA256}  /tmp/ceesafec.tar.gz" | sha256sum -c - \
 && mkdir -p /opt/ceesafec && tar -xzf /tmp/ceesafec.tar.gz -C /opt/ceesafec && rm /tmp/ceesafec.tar.gz \
 && ln -s /opt/ceesafec/ceesafec /usr/local/bin/ceesafec \
 && ceesafec --version | grep -qx "ceesafec ${CEESAFEC_VERSION}"

# The memory-order litmus job: herdtools7 is in Debian sid, not trixie.
FROM public.ecr.aws/docker/library/debian:sid@sha256:c53e050391cca6c8517c4a075230b798a0f980faa5d3ed0a912249ea882e40b5 AS litmus
LABEL org.opencontainers.image.source=https://github.com/vkuttyp/CosmoOS
LABEL org.opencontainers.image.description="CosmoOS CI litmus image (herdtools7)"
RUN apt-get update \
 && apt-get install -y --no-install-recommends herdtools7 python3 ca-certificates git \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /usr/share/cosmoos-ci \
 && dpkg-query -W -f '${Package} ${Version}\n' > /usr/share/cosmoos-ci/packages.txt
