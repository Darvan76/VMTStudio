# Release readiness — v0.2.2

Assessment date: 2026-11-16. This document covers the working tree prepared for
the next release after the published v0.2.1. It does not imply a Git push, tag or
GitHub release has been created.

Verdict: prepared as a release candidate. Build, automated tests and package
artifact validation pass. Complete the manual checks below before promoting it
as a stable release.

## Scope and release fixes

- Windows x64 Qt frontend and Win32 HLMV backend, C++17 / MSVC v143.
- Preserve the existing stability and performance changes in the working tree.
- Fix updater path handling and staging ownership, portable argument forwarding,
  simultaneous extraction, and the renderer readiness timeout.
- Generate the application version and Windows manifest from CMake 0.2.2.
- Make builds fail on native command errors and configure VTFLib Debug names
  consistently. CI pins the tested Source SDK headers.
- Include dependency notices and validate executable versions, PE architectures,
  Qt / CRT runtimes, preview models and exact ZIP content hashes.
- Build both configurations in CI; publish the verified Release ZIP, portable
  executable and SHA256SUMS only for an exact `v<project-version>` tag.

## Verification

| Check | Result |
| --- | --- |
| Source/resource integrity | Passed: 128 CMake entries, 10 Qt resources and all 8 shader schemas |
| Win32 HLMV and x64 application builds | Passed in Release and Debug with MSVC 2022 |
| Release CTest | 8 / 8 suites passed (8.49 seconds) |
| Debug CTest | 8 / 8 suites passed (11.23 seconds) |
| Windows ProductVersion | `0.2.2`, read from the built executable |
| Release ZIP verification | Passed: 61 staged files, matching ZIP content hashes, required runtimes, architectures and licenses |
| Debug ZIP verification | Passed: 69 staged files and matching ZIP content hashes (60,846,830 bytes) |
| Portable executable and checksums | Built successfully: `VMT-Studio-v0.2.2-Portable.exe` (24,951,808 bytes) and `SHA256SUMS` |
| Final artifact integrity | Both SHA-256 entries verified; the portable's embedded ZIP matches the distributed ZIP byte for byte (24,544,221 bytes) |
| Workflow / script syntax | YAML, PowerShell scripts and workflow run blocks parsed successfully |
| Git diff whitespace | Passed, respecting Windows line endings |
| Visual launch from the distribution | Not completed: Computer Use app approval timed out before launch |

MSBuild required execution outside the restricted sandbox to create its worker
processes. Third-party CMake emitted existing deprecation / NASM warnings, and
`windeployqt` warned that `VCINSTALLDIR` was unset. Packaging explicitly supplied
the app-local VC++ runtimes and then verified the required files. This was a
local build with the existing dependency checkout, not a fresh GitHub runner
execution; the updated workflow has not yet run remotely.

### Current branch verification before push

The current branch is `codex/l4d2-support` and also includes the later detection,
compatibility guards and diagnostics described in `L4D2_COMPATIBILITY.md`.
Before pushing, both Win32 and x64 builds were checked again: Release passed all
8 CTest suites in 8.09 seconds and Debug passed all 8 in 11.09 seconds. Source
integrity now covers 130 CMake source entries. The package sizes and hashes in
the table above refer to the earlier release-preparation snapshot; regenerate
artifacts from this branch before distributing these additional changes.
Native L4D2 rendering remains unimplemented.

## Supported scope and remaining manual checks

The renderer requires compatible x86 Source engine DLLs from the user's local
game. A successful unit test run does not establish compatibility with every
Source branch or GPU. Garry's Mod remains marked unsupported in the README.
Before promoting the candidate, run a real game session: connect, preview a
textured model and a brush, edit a material, save/reopen it, export model
materials, and reconnect/close with previews in progress. Run the downloaded
artifacts on a clean Windows 10/11 machine without Qt or Visual Studio installed.

The executables are unsigned. The updater's real network download and final
in-place replacement still require a published candidate; local tests exercise
release selection, preparation, cancellation, script paths and recovery.

## Build and package

Follow the dependency and SDK instructions in the root README first. Then:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Config Both -RunTests
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_single_exe.ps1 -UseExistingPackage
```

Outputs:

- `dist/vmtstudio-v0.2.2-win64.zip`
- `dist/VMT-Studio-v0.2.2-Portable.exe`
- `dist/SHA256SUMS`

Generated binaries and staging directories are ignored by Git. Keep the ZIP and
portable executable as release assets rather than committing them to the repo.

## GitHub publication

1. Review and commit the prepared source changes, including new regression tests,
   scripts, notices and license files. The working tree already contained local
   edits before this assessment; review the complete diff before committing.
2. Push the reviewed branch and wait for the Release and Debug jobs to succeed.
3. Use `docs/releases/v0.2.2.md` for the release description.
4. Once the manual checks above pass, create and push the annotated `v0.2.2` tag
   at that exact reviewed commit. The workflow checks the tag against CMake,
   rebuilds/tests both configurations, and publishes the Release assets only
   after successful jobs.
5. Verify the GitHub asset names and SHA-256 hashes, then smoke-test the downloaded
   ZIP and portable executable. Check an update from an older version separately.

No repository default branch change is required: CI accepts all push and PR
branches, including the current `codex/l4d2-support` branch and the
repository's existing `feature/game-manager-detection` default branch.
