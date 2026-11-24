# Working on the GT5 Pro kernel manifest

## Find the change

Start with the [module registry](.agents/MODULES.md). Match the paths and the behavior being changed. A config change in `build.sh` can also affect the module that consumes the config. Read the linked map for each affected module, then run `git log -10 --oneline -- <paths>` against its tracked paths. For an earlier design choice, search commit bodies with `git log --fixed-strings --grep='MODULE: <id>'`. Follow a patch into the common or vendor checkout when the diff leaves a specific question open.

## Verify the change

- Treat tracked patches, build settings and tests as the deliverables. `workspace/` contains generated or fetched trees. Inspect relevant local edits there before running `build.sh`, which resets and cleans several of those trees.
- Match the check to the change. Check shell syntax with `bash -n build.sh`. For a patch, check application against the intended source revision with its prerequisites, then exercise the affected behavior. Identify the build, guest or phone used for each result. For docs alone, verify the paths, commands and examples that changed.
- For a performance claim, record the workload, comparator, hardware or guest settings, sample count, units and aggregation. Use the affected module's map for measurement details.

## Leave a decision others can find

Review the staged diff and check the effective author and committer on shared build hosts (`git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT`). Use a Conventional Commit subject. In the body, describe the reason for the change, the checks actually run and their results.

For a design choice a later maintainer might revisit, add a `# Decisions` section. Give each decision a `MODULE:` from the registry and fill in `WHY:`, `ALTERNATIVES:`, `CHOSEN:`, `TRADEOFFS:` and `RISKS:`. Add `SUPERSEDES:` with a commit reference when replacing an earlier choice. Routine edits need only the relevant motivation and validation.

Update an affected map when a change alters an invariant, verification method or open issue. Keep one-off run results in the commit or PR. Add a map to the registry when a module accumulates context worth reusing.

These conventions adapt the [Appine contributor rules](https://github.com/mthli/appine/blob/865d24c1eb5469c05e6f88f78dde13150e26f43a/AGENTS.md).
