# The release version string — 2026-10-18

The project owner's task of 2026-10-18 ("The release version string"):
the published v1.0.4 says it is v1.0.3-123. Base: main `9c7ad9d8`.
Branch `release-version`.

## The defect

The published v1.0.4 kernel's build id is `v1.0.3-123-g1ec37307`, and its
`cosmo-upgrade` package's version `1.0.3-123` (v1.0.3's kernel said
`v1.0.2-45-g6011a5ec` the same way). The tag is annotated on GitHub
(`git cat-file -t v1.0.4` = `tag`, pointing at `1ec37307`), but
`actions/checkout`, on a tag push, fetches the tag by its commit
(`+1ec37307...:refs/tags/v1.0.4`, release run 39073532390). The clone's
tag is then lightweight, `git describe` (BUILD_ID, `build/config.mk`)
skips lightweight tags, and names the previous annotated one.

Reproduced in a clone with v1.0.4 made lightweight at `1ec37307`: `git
describe --always --dirty` says `v1.0.3-123-g1ec37307`, the published
string exactly.

## Changes

- `scripts/release-tag.sh TAG`, a step after the checkout in the
  `build` and `acceptance` jobs: refuses a tag not of the form
  `vX.Y.Z[-rcN]`, fetches `refs/tags/TAG` from origin (which brings the
  tag object), refuses a tag that is lightweight on origin, and requires
  `git describe --always --dirty` to be the tag.
- `scripts/check-release-version.py --tag TAG [--notes N] SET...`: every
  describe-shaped string in the set's `kernel.elf` (the banner's and
  uname's build id) is the tag; the set's files are the set's names; the
  `cosmo-upgrade` package's version is the tag's (`package_version`);
  the notes say "CosmoOS X.Y.Z" for this version and no other. The
  `build` job runs it on its set after `make release`; `publish` runs it
  on both sets and the notes before `gh release create`.
- `tests/host/test_release_version.py` (`make host-test`): both scripts
  on synthetic sets and a local repository.

The asset names carry no version (`cosmoos-aarch64.img`), so the name the
check holds to the tag is the package's, `cosmo-upgrade-X.Y.Z`; a file
name outside the set's is refused so none can name another.

## Proof

Against the published v1.0.4 (its `kernel.elf` and `.cpk`, downloaded
from the release), the check fails on both architectures:

```
check-release-version: cosmoos-x86_64-kernel.elf: build id v1.0.3-123-g1ec37307, not v1.0.4
check-release-version: cosmoos-x86_64-cosmo-upgrade.cpk: package cosmo-upgrade-1.0.3-123, not cosmo-upgrade-1.0.4
check-release-version: cosmoos-aarch64-kernel.elf: build id v1.0.3-123-g1ec37307, not v1.0.4
check-release-version: cosmoos-aarch64-cosmo-upgrade.cpk: package cosmo-upgrade-1.0.3-123, not cosmo-upgrade-1.0.4
```

In the clone above, `release-tag.sh v1.0.4` fetched the annotated tag and
`git describe` then said `v1.0.4`.

The dry run on a release-candidate tag: below.
