# Pinning this fork from mag-recomp

[mag-recomp](https://github.com/tekulvw/mag-recomp) builds against exact revisions of this fork. This
document says how those revisions are published so that a public build stays reproducible.

## Rules

1. **Every revision mag-recomp depends on has an annotated tag** in the `mag-recomp/` namespace.
   Branches are not pins: they move, and commits that only a deleted branch reached can disappear.
2. **Tag names are `mag-recomp/<role>-<YYYY.MM.DD>`.** The date is the tagged commit's committer
   date. The namespace keeps them apart from upstream's `vX.Y.Z` release tags, which this fork also
   carries. Do not create plain `v*` tags here.
3. **Published tags are immutable.** Never move, re-point or delete one. A new revision for a role gets
   a new tag. If two revisions for the same role share a date, append `.2`, `.3`, …
4. **mag-recomp pins the full commit SHA as well as the tag.** The SHA is authoritative. Its tooling
   checks that `git rev-parse <tag>^{commit}` equals the pinned SHA and refuses to build on a
   mismatch.
5. **Published commits contain no private references.** Before a revision is pushed to a public branch
   or tagged, its commit messages must not contain session trailers, private repository names or
   private commit IDs. Rewrite the messages on a published copy rather than publishing the originals.
6. **Only verified work is tagged.** Diagnostics, experiments and work in progress stay local.
   Discrete fixes still go upstream as their own `fix/*` or `feat/*` branches and PRs.

## Roles

| Role | Used by mag-recomp for |
| --- | --- |
| `runtime` | Runtime SDK passed to CMake as `PS3RECOMP_DIR` |
| `lifter-spu` | Inventory and launcher/engine SPU lifting |
| `lifter-ppu` | PPU and firmware lifting |
| `spu-programs` | Lifting every embedded engine SPU program |

## Current tags

| Tag | Commit |
| --- | --- |
| `mag-recomp/runtime-2026.10.24` | `c062d984d57b80f8763f8679a135192a114765ac` |
| `mag-recomp/lifter-spu-2026.10.07` | `5244db92669b54831c6d3ebb7a96d58c4dbfa6df` (upstream `v0.13.0`) |
| `mag-recomp/lifter-ppu-2026.10.12` | `dfa7073d2e164aec6890866c4951d8dedc28619d` |
| `mag-recomp/spu-programs-2026.10.21` | `4c5c05f60a6ae428b16e5ff7075c24a5e4d282ab` |

`git ls-remote --tags https://github.com/tekulvw/ps3recomp 'mag-recomp/*'` lists what is published.
Update this table when you add a tag.

## Publishing a new revision

1. Integrate on the role's `mag-recomp/*` branch, for example `mag-recomp/runtime-<date>` (rebased
   onto upstream as fixes land there). Build it and run the mag-recomp checks against it.
2. Check the commit messages against rule 5.
3. Push the branch, then create and push an annotated tag:

   ```sh
   git tag -a mag-recomp/runtime-2026.11.02 <sha> -m "mag-recomp runtime SDK, <one-line summary>"
   git push origin mag-recomp/runtime-2026.11.02
   ```

4. In mag-recomp, open a PR that updates the tag and SHA in its pin table, titled
   `fix(deps): pin ps3recomp <role> to mag-recomp/<role>-<date>`, so the release changelog records
   the change.
5. Add the tag to the table above.

## Protecting tags

A repository tag ruleset should target `refs/tags/mag-recomp/*` and block deletion and non-fast-forward
updates, so that rule 3 is enforced by GitHub rather than by convention.
