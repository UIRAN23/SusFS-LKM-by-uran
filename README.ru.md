[English](README.md) · **Русский**

# SusFS LKM by uran

Загружаемый модуль ядра SUSFS (`v2.4.0`, `susfs_guard_lkm`) для arm64 Android GKI (`android12-5.10` – `android17-6.18`). Настроен под `android14-6.1` (OnePlus Ace 5): inline-хуки HooKern, оптимизации supercall/kstat/path и полные метаданные `.modinfo`.

**Сайт (RU/EN):** включите GitHub Pages для репозитория (см. [docs/](docs/)) и откройте `https://<ваш-ник>.github.io/<репозиторий>/`.

## Поддерживаемые KMI

- `android12-5.10`, `android13-5.10`
- `android13-5.15`, `android14-5.15`
- `android14-6.1`
- `android15-6.6`
- `android16-6.12`, `android17-6.18`

## Установка

- **Менеджер LKM (например, FinaSU):** скачайте `SusFS-LKM-by-uran-<версия>-<kmi>.ko` для своего KMI и загрузите в менеджере. В карточке модуля будет `SusFS LKM by uran (<версия>)`, версия (`v2.4.0-r1` у релиза, `v2.4.0-r1-<хэш>` у сборки CI) и автор `uran`.
- **ZIP-модуль KernelSU:** скачайте `SusFS-LKM-by-uran-<версия>.zip` (или ZIP под один KMI), прошейте в менеджере KernelSU и перезагрузитесь. Ранняя загрузка до `post-fs-data` модулей (`/data/adb/post-fs-data.d/00-susfs-guard-lkm.sh` + `early_load`) включена по умолчанию. Отключить: создать `/data/adb/modules/susfs_guard_lkm/no_early_load`.
- **Вручную:** `ksud insmod <файл>.ko`, а без KernelSU загрузчик `susfs_insmod` из ZIP (`bin/`). Выгрузка: `rmmod susfs_guard_lkm`.

## Сборка в GitHub Actions

Запустите единственный workflow **Build SusFS LKM by uran**:
- **`kernel`**: `universal` (все 8 KMI и ZIP) или один KMI.
- **`publish_release`**: создать GitHub Release (`v2.4.0-rN`) с `.zip` и всеми `.ko`.
- **`release_tag`**: свой тег (например, `v2.4.0-r2` или `r2`); если пусто, `v2.4.0-rN` увеличивается сам.

Изменения в `docs/` и в `*.md` сборку не запускают.

## Оригинал

Основано на [`inforcqb/susfs4ksu-lkm`](https://github.com/inforcqb/susfs4ksu-lkm). Описание узлов `/proc`: [PROC_INTERFACES.md](https://github.com/inforcqb/susfs4ksu-lkm/blob/main/PROC_INTERFACES.md) (в этом форке часть узлов может отличаться).

Также используются [susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu), [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra), [KallRecon](https://github.com/Dere3046/KallRecon) и [HooKern](https://github.com/Dere3046/HooKern).

## Лицензия

GPL-2.0, см. [LICENSE](LICENSE). Только для исследований и обучения, используйте на свой риск.
