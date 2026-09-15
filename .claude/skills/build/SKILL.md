---
name: build
description: Build LocalGrid firmware without flashing — every firmware type, chosen types or chip targets, or what named boards run. Use when compiling, checking a change builds warning-free on every target, or cleaning a stale build.
---

# Build firmware

`tools/build.py` builds from the `firmware` table in `tools/bench_devices.json`. Each entry gives a firmware type's project folder and the chip targets it supports. Building opens no serial port, so boards and grid time are untouched. To build and put the result on boards, use the `flash` skill.

## Steps

1. **Load ESP-IDF** in PowerShell: `. C:\esp\v6.1\esp-idf\export.ps1`.
2. **Pick the scope**:

   | Goal | Command |
   |---|---|
   | Everything: every firmware type for every target | `python tools/build.py` |
   | One firmware type, all its targets | `python tools/build.py --firmware node` |
   | One firmware type for one chip | `python tools/build.py --firmware tests --target esp32s3` |
   | What specific boards run | `python tools/build.py hosyond node-main` |
   | What every handheld or node runs | `python tools/build.py --role H` |
   | Start from scratch | add `--clean` |
   | See the matrix and each last result | `python tools/build.py --list` |

3. **Read the RESULT table.** Done when every row shows `OK` with a size and free-space figure.
4. **Before a change is done**, run the plain `python tools/build.py`. A shared component such as `lg_core`, `lg_crypto`, `lg_identity`, `lg_board`, or `lg_ui` compiles into several firmware types and both chips, so building one target leaves the others unproven.

## Layout and rules

- Output goes to `<project>/build-<target>/`, with the generated `<project>/sdkconfig.<target>` and a full log at `<project>/build-<target>.log`. Targets never share a folder, so switching chips needs no `set-target` dance after the first build.
- Warnings fail the build, as they do when flashing. Fix the warning; keep the warning level.
- Settings come from `sdkconfig.defaults` and `sdkconfig.defaults.<target>`. The generated `sdkconfig.<target>` wins over the defaults once it exists, so after editing a defaults file use `--clean` for that firmware type.
- New firmware type: add a project folder and a `firmware` entry (`project`, `targets`, `roles`, `description`, and the `verify_*` fields `flash.py` uses). No script change is needed.
- `build.py` and `flash.py` share one run lock (`tools/.flash.lock`). A running flash, possibly from another session, makes a build stop with `Another flash.py run (process N) is using the boards`; wait for it to finish.

## When a build fails

| Failure | Meaning and fix |
|---|---|
| `build has N warning(s)` | The first five are listed; the log has all of them. |
| `set-target failed` | The IDF environment is not loaded, or the project's `CMakeLists.txt` is broken. |
| Dependency download errors | A managed component (for example `lvgl/lvgl`) could not be fetched. The first build of a project needs Internet; later builds use `managed_components/`. |
| `file is being used by another process` | Something outside the lock is building the same folder, such as a manual `idf.py` run. Let it finish. |
| A setting from a defaults file has no effect | The generated `sdkconfig.<target>` predates the edit. Rebuild with `--clean`. |
