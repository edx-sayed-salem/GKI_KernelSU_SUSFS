# Fork layout: `main` mirrors upstream, `master` is ours

This fork keeps two long-lived branches:

| Branch | Purpose | Rules |
|---|---|---|
| `main` | Exact mirror of [`WildKernels/GKI_KernelSU_SUSFS`](https://github.com/WildKernels/GKI_KernelSU_SUSFS) | Never commit here. Fast-forward only, by `sync-upstream.yml` or the script below. |
| `master` | **Default branch.** Fork-only changes (16 KB autobuilds, workflow fixes, docs) | Merge `main` into it to pick up upstream. Feature work branches off here. |

Why `master` as default: scheduled GitHub Actions workflows only run from the default
branch. `build-16k.yml` and `sync-upstream.yml` live on `master`, so `master` must be the
repository default for the weekly runs to happen.

## One-time repository settings

1. **Default branch** — Settings → Branches → Default branch → `master`
   (or `gh repo edit --default-branch master`).
2. **Actions permissions** — Settings → Actions → General → Workflow permissions →
   **Read and write permissions** (the build creates releases and the sync pushes `main`).
3. **Enable workflows** if GitHub disabled them on fork creation.

## 16 KB autobuilds

`build-16k.yml` dispatches the upstream **Build Kernels** workflow (`main.yml`) with
`page_size=16k`; it does not duplicate the build matrix.

- **Weekly (Monday 03:17 UTC):** `6.18.x-android17` (KernelSU-Next, no SUSFS) and
  `6.12.x-android16` (KernelSU-Next + SUSFS), both `lts`, as pre-releases.
- **Manual:** Actions → *Build 16 KB Kernels* → Run workflow. Pick one family (6.1, 6.6,
  6.12, 6.18), patch level, root flavor, SUSFS and release type.
- `16k` requires an explicit family in the upstream workflow (its `All` option is rejected);
  that is why the workflow always passes one.
- 6.18 has no `susfs4ksu` `gki-android17-6.18` branch upstream, so SUSFS is forced off there
  and the workflow emits a warning.
- Artifacts land in the fork's **Actions** run; `Pre-Release`/`Release` also create an `rN`
  release. Asset names carry the `-16k` suffix.

## Keeping `main` in sync

Automated: `sync-upstream.yml` runs weekly and fast-forwards `main` to
`WildKernels/GKI_KernelSU_SUSFS:main`. If `main` has diverged, it fails instead of rewriting.

Manual equivalent:

```bash
git fetch upstream main
git push origin upstream/main:main        # fast-forward only; fails if main diverged
```

First-time remote setup:

```bash
git remote add upstream https://github.com/WildKernels/GKI_KernelSU_SUSFS.git
git fetch upstream
```

## Updating `master` from upstream

`sync-upstream.yml` only updates `main`; merging into the custom branch is deliberate:

```bash
git switch master
git fetch origin main
git merge --ff-only origin/main   # or a normal merge when master has commits
git push origin master
```

If the upstream workflow files changed, resolve conflicts here and keep the fork-only
workflows (`build-16k.yml`, `sync-upstream.yml`) plus the 6.18 page-size gate fix.

## Flashing a 16 KB kernel

A 16 KB build is only useful on a device whose userspace and boot image expect 16 KB pages
(e.g. Android 16/17 `ps16k` images). Keep the stock `boot.img`; verify with
`adb shell getconf PAGESIZE` after boot. KMI must match the device's stock kernel family.
