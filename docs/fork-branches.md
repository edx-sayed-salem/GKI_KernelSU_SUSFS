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

`build-16k.yml` (display name *Build Kernels (4 KB + 16 KB)*) dispatches the upstream
**Build Kernels** workflow (`main.yml`) once per page size; it does not duplicate the matrix.

- **Weekly (Monday 03:17 UTC):** `6.18.x-android17` (KernelSU-Next, no SUSFS) and
  `6.12.x-android16` (KernelSU-Next + SUSFS), both `lts`, **4 KB and 16 KB** — four runs.
- **Manual:** Actions → *Build Kernels (4 KB + 16 KB)* → Run workflow. Pick `both`, `4k` or
  `16k`, one family (6.1, 6.6, 6.12, 6.18), patch level, root flavor, SUSFS and release type.
- `16k` requires an explicit family in the upstream workflow (its `All` option is rejected);
  that is why the workflow always passes one.
- 6.18 has no `susfs4ksu` `gki-android17-6.18` branch upstream, so SUSFS is forced off there
  and the workflow emits a warning.
- `CI` runs upload **Actions artifacts only**. `Pre-Release`/`Release` create a release as
  well: 4 KB uses the plain `rN` line, 16 KB uses `rN-16k` (title "Release N (16 KB)"), so
  both can run at once without racing for the same tag.
- 16 KB assets carry the `-16k` suffix in their filenames.

## First verified pair (2026-11-07)

Manual `both` / `6.18.x-android17` / `lts` / KernelSU-Next / no SUSFS / Pre-Release:

- `r1` — 4 KB: `6.18.38-android17-lts-KernelSU-Next-AnyKernel3.zip`
- `r1-16k` — 16 KB: `6.18.38-android17-lts-KernelSU-Next-16k-AnyKernel3.zip`

Embedded kernel config extracted from the two `Image` files:

```
4k : CONFIG_ARM64_4K_PAGES=y   # CONFIG_ARM64_16K_PAGES is not set
16k: CONFIG_ARM64_16K_PAGES=y  # CONFIG_ARM64_4K_PAGES is not set
```

Both identify as `Linux version 6.18.38-android17-Wild`; the `Image` hashes differ
(`2ece8786…` vs `1884ede9…`). Each release also carries the two manager APKs and
`NoMount-Metamodule.zip`.

Second pair, matching the Android 17 `ps16k` emulator's `6.12.x-android16` KMI, with SUSFS:

- `r2` — 4 KB: `6.12.97-android16-lts-KernelSU-Next-AnyKernel3.zip`
- `r2-16k` — 16 KB: `6.12.97-android16-lts-KernelSU-Next-16k-AnyKernel3.zip`
- SUSFS `v2.3.0` (`c6788e2a…`), KernelSU-Next `v3.4.2-130-g40f77cdb`, configs
  `CONFIG_ARM64_4K_PAGES=y` / `CONFIG_ARM64_16K_PAGES=y` respectively; `Image` hashes differ.

## 16 KB manager APKs (paired signing)

A 16 KB kernel is not enough: the KernelSU manager APK packages `ksud` as
`lib/arm64-v8a/libksud.so` and **execs it** (`getKsuDaemonPath()` → `nativeLibraryDir/libksud.so`).
Upstream's `ksud.yml` only passes `-Wl,-z,max-page-size=16384` for riscv64, so the aarch64
`libksud.so` in upstream manager APKs is `p_align=0x1000` and cannot run on a 16 KB-page device.
That is why the manager reports **"Not installed"** there even though the kernel has
`CONFIG_KSU=y`.

The fork builds its own managers:

| Repo | `main` | `master` (default) |
|---|---|---|
| `edx-sayed-salem/KernelSU-Next` | upstream `dev` | + 16 KB `ksud.yml` flags + manager-cert pin |
| `edx-sayed-salem/BakaSU` | upstream `main` | + 16 KB `ksud.yml` flags + manager-cert pin |

Two things must match, or the kernel will never crown the app:

1. **Page alignment.** `libksud.so` `LOAD` segments must be `0x4000` (verify with
   `readelf -lW`). `libkernelsu.so` / `libadbroot.so` / the embedded `ksuinit` were already
   16 KB upstream.
2. **Manager certificate.** The kernel pins the manager's signing certificate
   (`KernelSU-Next`: `KSU_NEXT_MANAGER_SIZE/HASH`, or `KSU_NEXT_MANAGER_LIST` on pershoot's
   SUSFS fork; `BakaSU`: `EXPECTED_SIZE_BAKASU/HASH_BAKASU`). Both are the **certificate DER
   length and SHA-256**, not the APK size/hash. Our managers are signed with the project
   keystore:

   ```
   size 0x320 (800 bytes)
   sha256 bfdd092457348ee7cb7f6e228b8fe518e95d445b0736d796d578a5e3b478940d
   DN CN=KernelSU Manager, O=KernelSU, C=US
   ```

   The cert lives in the repo forks' `master` and, for the proven r22 tree, is injected by
   `build.yml` after `root-setup` (`r22-16k` branch). A re-signed APK with any other key is
   rejected by `is_manager_apk()` and shows "Not installed".

Verify a built manager:

```bash
unzip -p <apk> lib/arm64-v8a/libksud.so > /tmp/libksud.so
readelf -lW /tmp/libksud.so | awk '/LOAD/{print $0}'   # Align must be 0x4000
apksigner verify --print-certs <apk>                   # digest must match the kernel pin
```

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
