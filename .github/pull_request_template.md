## Description

Please provide a brief summary of the changes introduced in this pull request and the rationale behind them.

Fixes #(issue)

---

## Type of Change

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance optimization (improves parsing, rendering, or memory efficiency)
- [ ] Refactoring / Architecture improvement
- [ ] Documentation update
- [ ] CI/CD or build system change

---

## Architectural Considerations

- [ ] **Decoupling**: `vmt_core` has zero dependencies on Qt, Windows API, or Source SDK.
- [ ] **VTFLib++ Isolation**: VTFLib headers remain private to `vtf_reader`.
- [ ] **Dual-Process Compliance**: 64-bit frontend (`vmtstudio`) communicates with 32-bit backend (`hlmv_backend`) strictly via IPC.
- [ ] **CRT Consistency**: Static CRT for Win32 standalone target; dynamic CRT for Qt 6 x64 frontend.

---

## Testing & Verification

Please describe the tests you ran to verify your changes:

- [ ] Ran 64-bit unit test suite: `ctest --test-dir build64 -C Release --output-on-failure` (100% passing)
- [ ] Ran 32-bit unit test suite: `ctest --test-dir build -C Release --output-on-failure` (if Win32 tests were built)
- [ ] Verified live 3D preview synchronization with embedded HLMV backend
- [ ] Tested with real VMT/VTF assets from retail Source Engine games (e.g. HL2, TF2, CS:S)
- [ ] Zero compiler warnings (`/W4 /WX` enabled)

---

## Screenshots / Media (if applicable)

*If this PR modifies UI layout, rendering shaders, or property inspectors, please attach before/after screenshots or GIFs.*

---

## Checklist

- [ ] My code follows the style guidelines of this project.
- [ ] I have performed a self-review of my own code.
- [ ] I have commented my code, particularly in hard-to-understand areas.
- [ ] I have added tests that prove my fix is effective or that my feature works.
- [ ] New and existing unit tests pass locally with my changes.
