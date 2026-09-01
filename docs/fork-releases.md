# Fork release process

How to publish builds from `mittermichal/etlegacy`.

Upstream's `ETLBuild` (`.github/workflows/build.yml`) cannot publish from a fork: its
`sign` job is gated on `github.repository == 'etlegacy/etlegacy'`, and `upload` needs
`sign`, so everything downstream is skipped. It also uploads to etlegacy.com through
secrets a fork does not have, and never creates a GitHub Release at all.

The fork therefore has its own workflow: **`.github/workflows/fork-release.yml`**
(`ForkRelease`). It builds only the mod libraries and publishes them as workflow
artifacts and, optionally, a GitHub Release.

## What it builds

`cgame`, `ui`, `qagame`, `tvgame` for:

| Job | Platform | Manifest string |
| --- | --- | --- |
| `lnx64-mod` | Linux x86_64 (always built) | `lnx_x86_64` |
| `lnx32-mod` | Linux x86 | `lnx_x86` |
| `lnx-aarch64-mod` | Linux aarch64 (cross-compiled) | `lnx_armv8_64` |
| `win-mod` | Windows x86 | `win_x86` |
| `win64-mod` | Windows x86_64 | `win_x86_64` |
| `osx-mod` | macOS universal | `macos_x86_64` + `macos_aarch64` |

**Android is not built, and cannot be** without work: the release path needs the
`ANDROID_SIGN_APK_ALLIAS` / `ANDROID_SIGN_APK_PASS` secrets, and upstream's `android-mod`
job is broken in forks anyway — it falls back to `assembleDebug` when the repository is
not `etlegacy/etlegacy` (build.yml:286-297), but the artifact step still globs
`merged_native_libs/*/mergeReleaseNativeLibs/...`, which then matches nothing.

The `etl` / `etlded` engine binaries are also not built — mod libraries only.

`lnx64-mod` is not optional: it is the only job that produces the base
`incomplete-mod-pk3` that everything else is merged into.

Outputs, both as artifacts and as release assets:

- `All-mods` — `legacy_<version>.pk3` with every platform's `cgame`/`ui` plus
  `platforms.manifest`
- `mod-zip` — `etlegacy-mod-<describe>.zip` containing that pk3 plus the
  `qagame`/`tvgame` binaries

## Running it

Actions → **ForkRelease** → *Run workflow*.

- **Use workflow from** picks the branch. The run checks out that ref *and executes the
  copy of the workflow file on it* — so an experimental branch must be cut from (or
  rebased onto) a `master` that already contains `fork-release.yml`. The workflow only
  appears in this dropdown because it exists on the default branch.
- `lnx32` / `lnx_aarch64` / `osx` / `win32` / `win64` — deselect to skip a platform.
  `platforms.manifest` is generated from these, so the pk3 never advertises support it
  does not contain.
- `release_tag` — leave empty for artifacts only. Set it to publish a GitHub Release,
  created at the run's commit (`--target <sha>`). See the tag rules below.
- `prerelease` — leave on for anything that is not a considered release.

Artifacts require the downloader to be signed into GitHub and expire with the retention
period. Release assets are anonymous, permanent URLs — use a release for anything you
want to hand to other people. Deleting the branch afterwards is fine; the tag keeps the
commit reachable.

Re-running with a `release_tag` that already exists **fails** rather than updating the
release. Pick a new tag, or delete the old release and tag first.

## Tag naming — keep the `v<major>.<minor>.<patch>` prefix

**Use `v2.85.0-kimi1`, not `exp-mybranch-20260901`.**

`git describe` resolves the *nearest reachable* tag, so a badly-named tag poisons every
later build on that branch, not just the tagged one. Three places parse the resulting
version string and all of them assume the `v<M>.<m>.<p>` shape:

- `Com_ParseUA()`, `src/qcommon/common.c:4681` — `Q_sscanf(..., " v%2i.%3i.%2i-%4i-*")`,
  then `if (ETL_VERSION(2, 81, 1, 0) <= versionInt)` to set the auth-capable bit. An
  unparseable string leaves all four components 0 and drops the client to basic
  compatibility.
- `MOD_CHECK_ETLEGACY`, `src/qcommon/q_shared.h:120` — compares the first 13 chars of the
  `etVersion` cvar against `"ET Legacy v2."`. This is how mod code detects an ET:L client
  at all.
- `cmake/ETLVersion.cmake:135` — everything is gated behind
  `MATCHES "^v[0-9]+\\.[0-9]+.*"`; otherwise the build silently falls back to the
  `VERSION.txt` numbers.

`v2.85.0-kimi1` behaves correctly: a later commit describes as `v2.85.0-kimi1-3-gabc1234`,
CMake extracts major/minor/patch/commit, and `Com_ParseUA` reads `2.85.0` (the commit
count is lost to the `-kimi1` suffix, which is harmless) so the ≥ 2.81.1 check passes.

## Versioning notes

`pre-build` computes two values from git and passes them to every job:

- `CI_ETL_DESCRIBE` (`git describe --abbrev=7`, e.g. `v2.85.0-49-g631d0c9`) →
  `ETL_CMAKE_VERSION` → `ETLEGACY_VERSION` in the game. Used in the zip filename.
- `CI_ETL_TAG` (`git describe --abbrev=0`, e.g. `v2.85.0`) → `ETL_CMAKE_VERSION_SHORT`,
  used in the pk3 filename.

The Linux jobs pass `CI=true`, which makes `cmake/ETLVersion.cmake:127-130` force
`ETL_CMAKE_VERSION_SHORT` to the full describe for non-release builds. That is what keeps
the pk3 filename in step with what the autoupdater expects
(`legacy_<ETLEGACY_VERSION>.pk3`, `src/qcommon/update.c:104`).

If `git describe` returns nothing, `pre-build` falls back to `<VERSION.txt>-fork` so the
build cannot produce a nameless `etlegacy-mod-.zip`.

## Maintenance

`build.yml` and the other upstream workflows are deliberately untouched, so syncing with
upstream stays conflict-free. Sync with `git rebase origin/master` or a merge — never
`git reset --hard origin/master`, which would drop the fork-only workflow.

Two upstream details to be aware of if you ever extend this workflow into engine builds:

- The `win`/`win64` engine jobs in `build.yml` run SignPath steps with **no** repository
  guard (build.yml:629, 705); they fail without `SIGNPATH_*` secrets.
- The mod merge in `build.yml` includes `platforms/libcgame*` / `platforms/libui*` — the
  Android naming. `zip -MM` aborts on a pattern that matches nothing, so those patterns
  are omitted here.

`easybuild.sh` reads `BUILD_CLIENT` / `BUILD_SERVER` / `BUILD_MOD` from the environment
(easybuild.sh:501-504), so an engine-only or server-only build needs no new flags there.
`easybuild.bat` hardcodes them (easybuild.bat:425-426) and would need a change.
