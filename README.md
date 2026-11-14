# SusFS LKM by uran

SUSFS (`v2.4.0`) loadable kernel module (`susfs_guard_lkm`) for arm64 Android GKI kernels (`android12-5.10` – `android17-6.18`), tuned for `android14-6.1` (OnePlus Ace 5) with HooKern inline hooks, supercall/kstat/path optimizations, and full `.modinfo` build metadata.

## Supported KMI targets

- `android12-5.10`, `android13-5.10`
- `android13-5.15`, `android14-5.15`
- `android14-6.1`
- `android15-6.6`
- `android16-6.12`, `android17-6.18`

## Installation

- **Managed LKM loader (e.g. FinaSU):** download `SusFS-LKM-by-uran-<version>-<kmi>.ko` for your kernel KMI and load it in the LKM manager. Module details show `SusFS LKM by uran (<version>)`, exact CI/release version tag, and author `uran`.
- **KernelSU ZIP module:** download `SusFS-LKM-by-uran-<version>.zip` (or single-KMI ZIP), flash in KernelSU manager, and reboot. Early loading before module `post-fs-data` can be enabled with `/data/adb/modules/susfs_guard_lkm/early_load`.

## Building via GitHub Actions

Run the single **Build SusFS LKM by uran** workflow:
- **`kernel`**: `universal` (all 8 KMIs + ZIP) or a single KMI.
- **`publish_release`**: create a GitHub Release with the `.zip` and all `.ko` files attached.
- **`release_tag`**: optional custom release tag (defaults to `VERSION`).

## Upstream

Based on [`inforcqb/susfs4ksu-lkm`](https://github.com/inforcqb/susfs4ksu-lkm).
