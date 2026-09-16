# Versioning and rollback

Keep `main` runnable and use annotated tags for hardware-verified states. A normal GitHub clone is already initialized; do not run `git init` inside it. Update a clean checkout with:

```bash
cd ~/src/rk3588-hdmi-lowlatency
git switch main
git status --short
git pull --ff-only
```

`git status --short` should be empty before pulling. After the four-buffer normal path has been verified on hardware, mark that exact commit once:

```bash
git tag -a v3.4-known-good -m "Known-good four-buffer explicit-sync baseline"
git push origin v3.4-known-good
```

The V3.5 code is included, but its normal mode is the known-good V3.4 path and `--async-flip` remains experimental. Test each hypothesis on a branch created from the verified tag:

```bash
git switch -c experiment/v3.5-async v3.4-known-good
sudo ./scripts/run-v35-phase.sh
git add docs/results
git commit -m "Record V3.5 async result"
```

If the experiment fails or the board becomes unstable, return without rewriting history:

```bash
git switch main
git branch -D experiment/v3.5-async
git switch --detach v3.4-known-good
```

The last command checks out the exact baseline for reproduction. Switch back with `git switch main`.

When an experiment is proven:

```bash
git switch main
git merge --no-ff experiment/v3.5-async
git tag -a v3.5-async-result -m "Hardware-verified V3.5 async result"
```

Push branches and tags explicitly when the remote is ready:

```bash
git push -u origin experiment/v3.5-async
git push origin --tags
```

For the current profiler release, upload/merge the complete V3.6 tree to `main`, verify it on the board, then tag the exact verified commit:

```bash
git tag -a v3.6-phase-profiler -m "Hardware-verified V3.6 phase profiler"
git push origin v3.6-phase-profiler
git switch -c experiment/v3.7-refresh-offset v3.6-phase-profiler
```

Do not create the V3.6 tag before the Orange Pi run passes; tags identify hardware-verified states, not merely uploaded code.

V3.7 changes output timing temporarily, so retain `v3.6-phase-profiler` as the immediate rollback point. After the A/B runner completes, verify the original mode-restoration message before tagging:

```bash
git tag -a v3.7-cadence-alignment -m "Hardware-verified V3.7 cadence result"
git push origin v3.7-cadence-alignment
```

Avoid using patch-application scripts, force-pushes, or untagged “known good” states as the rollback mechanism.
