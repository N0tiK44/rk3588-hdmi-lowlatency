# Versioning and rollback

Keep `main` runnable and use annotated tags for hardware-verified states. The ZIP does not contain a hidden `.git` directory, so initialize history after extracting it:

```bash
git init
git add .
git commit -m "Consolidated RK3588 HDMI-RX baseline"
git branch -M main
git tag -a v3.4-known-good -m "Known-good four-buffer explicit-sync baseline"
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
git remote add origin YOUR_NEW_REPOSITORY_URL
git push -u origin main
git push origin --tags
```

Avoid using patch-application scripts, force-pushes, or untagged “known good” states as the rollback mechanism.
