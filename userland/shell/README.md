# userland/shell

The native shell, `sh` (`/bin/sh`, docs/userland/design.md). Prompt
`cosmo$ ` (the constitution's `myos$` with the project's name). Words
with quotes and escapes, `$VAR` `${VAR}` `$?` `$$` `$0..$9` `$#`,
pipelines, `<` `>` `>>` `2>` `2>&1`, `;` `&&` `||`, assignments,
builtins (`cd pwd exit export unset set : true false wait . source`),
programs through `spawnvp` with an explicit handle map per pipeline
stage (there is no fork). `sh file [args]`, `sh -c 'cmd'`, `-e`. No
control flow, globbing, jobs or command substitution yet.
