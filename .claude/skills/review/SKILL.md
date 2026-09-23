---
description: Review a change in this repository with CosmoReview before opening a pull request — the deterministic rules plus the analysts, run locally on this machine's own Claude Code sign-in. Use when asked to review a change, check a diff, look over work before a PR, or see what the reviewer says about a branch.
allowed-tools: ["Bash", "Read"]
---

# Reviewing a change with CosmoReview

CosmoReview is the reviewer in `~/dev/CosmoReview`. It already reviews every pull request here from the server and
posts its findings on GitHub. This runs the same engine locally, before the pull request exists, and posts nothing.

## The command

```bash
REVIEWER=~/dev/CosmoReview/src/CosmoReview.Cli/bin/Debug/net10.0/reviewer
"$REVIEWER" index . -s sqlite:/tmp/cosmoos-index.db
"$REVIEWER" review -C . -s sqlite:/tmp/cosmoos-index.db --base main --provider claude-code --model opus
```

If that binary is missing, build it once with
`dotnet build ~/dev/CosmoReview/src/CosmoReview.Cli`, or substitute
`dotnet run --project ~/dev/CosmoReview/src/CosmoReview.Cli --` for `"$REVIEWER"`, which rebuilds each time and is
slower.

## Four things that will otherwise waste a run

- **Commit first.** The review refuses a dirty working tree, because the index and the commit-to-commit patch
  cannot see uncommitted edits. It says so and stops.
- **Index after the last commit, not before.** The head revision must be the tree that was indexed. Re-indexing is
  incremental and takes about twelve seconds on this repository.
- **Keep the store out of the tree.** `-s sqlite:/tmp/...` stops a `.reviewer/` directory appearing in the working
  copy.
- **`--base main` is enough.** The reviewer takes the merge base with the head, so a long-lived branch is still
  reviewed on its own changes rather than on everything `main` has done since.

## What it costs

Each review takes five to ten minutes and spends a real share of this machine's Claude Code session window: the
analysts run on the subscription, one call at a time, and `reviewer.yaml` here sets the pace deliberately. Run it
when a change is ready to be read, not after every edit. Say so before starting one, because the person may be
using that window for something else.

## Reading the result

The run prints each finding with its evidence, the deterministic checks, the verifier's verdict and any suggested
patch. `"$REVIEWER" reviews -C . --id <id>` shows a stored review again; `"$REVIEWER" comments -C . --format github`
renders the findings as review comments, which is only useful if somebody is going to paste them.

A finding is a claim with evidence attached. Check the cited lines before acting on one, and say plainly when the
reviewer is wrong — it is wrong often enough to be worth reading sceptically, and the cited lines are there so that
can be checked rather than guessed.
