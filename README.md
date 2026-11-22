**English** · [Русский](README.ru.md)

# SusFS LKM by uran

SUSFS (`v2.4.0`) loadable kernel module (`susfs_guard_lkm`) for arm64 Android GKI kernels (`android12-5.10` – `android17-6.18`), tuned for `android14-6.1` (OnePlus Ace 5) with HooKern inline hooks, supercall/kstat/path optimizations, and full `.modinfo` build metadata.

**Website (EN/RU):** enable GitHub Pages for this repo (see [docs/](docs/)) and open `https://<your-name>.github.io/<repo>/`.

## Supported KMI targets

- `android12-5.10`, `android13-5.10`
- `android13-5.15`, `android14-5.15`
- `android14-6.1`
- `android15-6.6`
- `android16-6.12`, `android17-6.18`

## Installation

- **Managed LKM loader (e.g. FinaSU):** download `SusFS-LKM-by-uran-<version>-<kmi>.ko` for your kernel KMI and load it in the LKM manager. Module details show `SusFS LKM by uran (<version>)`, version (`v2.4.0-r1` for releases or `v2.4.0-r1-<hash>` for CI), and author `uran`.
- **KernelSU ZIP module:** download `SusFS-LKM-by-uran-<version>.zip` (or single-KMI ZIP), flash in KernelSU manager, and reboot. Early loading before module `post-fs-data` (via `/data/adb/post-fs-data.d/00-susfs-guard-lkm.sh` + `early_load`) is enabled by default, and can be disabled by creating `/data/adb/modules/susfs_guard_lkm/no_early_load`.
- **Manual:** `ksud insmod <file>.ko`, or the `susfs_insmod` loader from the ZIP (`bin/`) on non-KernelSU devices. Unload with `rmmod susfs_guard_lkm`.

## Building via GitHub Actions

Run the single **Build SusFS LKM by uran** workflow:
- **`kernel`**: `universal` (all 8 KMIs + ZIP) or a single KMI.
- **`publish_release`**: create a GitHub Release (`v2.4.0-rN`) with the `.zip` and all `.ko` files attached.
- **`release_tag`**: optional custom release tag (e.g. `v2.4.0-r2` or `r2`; auto-increments `v2.4.0-rN` if empty).

Changes under `docs/` and `*.md` files do not start a build.

## Upstream

Based on [`inforcqb/susfs4ksu-lkm`](https://github.com/inforcqb/susfs4ksu-lkm). `/proc` node documentation: [PROC_INTERFACES.en.md](https://github.com/inforcqb/susfs4ksu-lkm/blob/main/PROC_INTERFACES.en.md) (some nodes may differ in this fork).

Also built on [susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu), [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra), [KallRecon](https://github.com/Dere3046/KallRecon) and [HooKern](https://github.com/Dere3046/HooKern).

## License

GPL-2.0, see [LICENSE](LICENSE). For research and study only; use at your own risk.
