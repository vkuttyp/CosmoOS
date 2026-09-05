# ports

Declarative, reproducible package recipes (docs/pkg/). One directory per
package version with a `port` file (`name: version: summary: depends:
prefix: program: file:`) and its sources; `make ports` cross-compiles
them with the userland toolchain into signed `.cpk` packages and a
signed `INDEX` under `$(OUT)/pkg/repo/`, which the boot archive carries
as `repo/` (`/boot/repo` on the target). Today: `hello` (1.0 and 1.1),
`fortunes` (data) and `fortune` (depends on `fortunes`).
