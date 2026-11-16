# Audit cleanup development plan

Implement the nine audit findings as a local, behavior-preserving cleanup. The
audit estimated roughly 1,050 removable lines and no dependency removals; actual
results will be measured after integration.

## Workstreams and ownership

| Owner | Scope | Deliverable |
| --- | --- | --- |
| Browser/cache agent | `material_browser`, `asset_cache`, affected UI tests | Remove the superseded MaterialIndex/BMP cache and unused TSV file wrappers; retain path normalization and packed-cache serialization/parsing. |
| Bridge agent | EngineView, backend IPC, protocol documentation | Remove unused material-thumbnail IPC and material-list APIs; retain generalized asset listing, texture thumbnails and model `-thumb` capture. |
| CI/package agent | GitHub Actions workflow, release packaging script | Merge Debug/Release jobs into a configuration matrix; remove unconsumed x86 VTFLib packaging and external schema copies. |
| Coordinator | Module scaffolding, CMake, Result helpers, smoke tests, integration | Remove the empty renderer target, dummy module identities and unused Result conveniences; preserve meaningful compile/link and engine-view coverage. |

The agents edit separate files. The coordinator owns shared CMake files and
`tests/skeleton_test.cpp` to avoid conflicting edits. Existing local changes in
the packaging and portable-launcher files must be preserved.

## Implementation sequence

1. Recheck each finding against current call sites before deleting it. Keep any
   code that has a current product consumer.
2. Run the three agent workstreams in parallel while the coordinator removes
   scaffolding and unused Result helpers.
3. Integrate changes, update affected tests and documentation, and search for
   references to every removed API/target.
4. Configure and build the x64 Qt application/tests and Win32 HLMV backend.
5. Run existing CTest coverage, source-integrity checks, and workflow/script
   validation. Exercise packaging when the available local tools allow it.
6. Record measured changes and validation results here; leave the completed
   changes local for review.

## Acceptance criteria

- Current asset browsing, packed cache, embedded schemas and model thumbnails
  retain their implementation and test coverage.
- All six existing CTest targets pass after a fresh build.
- The Win32 HLMV backend and x64 Qt frontend compile successfully.
- CI still builds both configurations on push/PR, honors manual Release/Debug/
  Both selection, and preserves existing release-publishing conditions.
- Packaging retains required x64 VTFLib, Qt and HLMV assets and the user's
  existing local changes.
- No new dependencies, language-standard upgrades or replacement abstractions.

## Progress

- [x] Audit findings and ownership captured.
- [x] Browser/cache cleanup.
- [x] Bridge API cleanup.
- [x] CI/package cleanup.
- [x] Scaffolding and Result cleanup.
- [x] Combined build and validation.

## Validation results

The three agent workstreams and coordinator cleanup are implemented locally.
Measured against the pre-task working files: **1,328 net lines removed** from
tracked files, excluding this plan. No dependencies were added or removed.
Pre-existing launcher changes were untouched; the packaging diff was checked
against its pre-task snapshot and retains the user's existing changes.

| Check | Result |
| --- | --- |
| x64 Release application and test build | Pass |
| Win32 Release and Debug HLMV builds | Pass |
| Release CTest | All 6 targets pass |
| x64 Debug application and test build | Pass |
| Debug CTest | All 6 targets pass |
| Source integrity | 95 CMake source entries and all 10 resources, including 8 schemas, pass |
| Workflow and scripts | YAML, 7 workflow PowerShell blocks and package script parse; 64 selection/publication scenarios pass |
| Release packaging | Pass: 49 files, 25,402,524-byte ZIP; CRCs and required app/engine/Qt/CRT/model files verified |
| Removed payloads and APIs | Obsolete x86/schema payloads absent from ZIP; removed APIs and targets have no remaining code references |
| Whitespace check | Pass |

The verification archive is
`scratch/ponytail-package-release/vmtstudio-v0.2.1-win64.zip`; build/test logs are
under `scratch/ponytail-*.log`. No commit, push or remote release was made.

The sandbox denies `GetFinalPathNameByHandleW`, which MSVC uses for filesystem
path resolution. Final CTest verification therefore runs outside the sandbox
with temporary files confined to `scratch/ponytail-temp`. A transient failure
in the existing 200 file-replacement stress cycles cleared after compilation
finished; production file-writing logic and its safety tests were retained.
Win32 builds report SDK/CRT linker warnings (LNK4217 and Debug LNK4098); Qt's
deployment tool also warns that `VCINSTALLDIR` is unset. Both builds complete.
Remote GitHub Actions and a game-connected rendering session were not run.
