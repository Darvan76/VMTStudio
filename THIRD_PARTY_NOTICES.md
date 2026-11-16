# Third-party notices

The root MIT license applies to original VMT-Studio code. It does not replace
the licenses of the components below. Binary distributions include their notices
under `licenses/`. Original copyright notices remain in the source files.

| Component | License / notice | Source used |
| --- | --- | --- |
| Qt 6.8.2 (Core, Gui, Widgets, Network and runtime plugins) | LGPL-3.0 or applicable Qt license; see `licenses/Qt-LGPL-3.0.txt` and `licenses/Qt-GPL-3.0.txt` | [Qt 6.8.2 source](https://download.qt.io/archive/qt/6.8/6.8.2/single/) |
| VTFLib++ | LGPL-2.1; see `licenses/VTFLib-LGPL-2.1.txt` | [Pinned source b27ecd36736fb916c5b5ec8574b4299303c66169](https://github.com/ficool2/VTFLib/tree/b27ecd36736fb916c5b5ec8574b4299303c66169) |
| miniz, linked into VTFLib++ | MIT; see bundled miniz license | Included in the pinned VTFLib submodule |
| Zstandard, linked into VTFLib++ | BSD license; see bundled Zstandard license | Included in the pinned VTFLib submodule |
| AMD Compressonator, linked into VTFLib++ | MIT and bundled dependency notices; see Compressonator license | Included in the pinned VTFLib submodule |
| Valve Source SDK 2013 / HLMV derived code and SDK libraries | Source 1 SDK License and third-party notices | [SDK source b8cfb12c0e083a2ef5b2f9f9b50f3902fa034474](https://github.com/ValveSoftware/source-sdk-2013/tree/b8cfb12c0e083a2ef5b2f9f9b50f3902fa034474); see `licenses/Valve-Source-SDK-LICENSE.txt` and `licenses/Valve-thirdpartylegalnotices.txt` |
| mxToolKit | Mete Ciragan's freely distributable notice | `hlmv_backend/mxtk`; see `licenses/mxToolKit-notice.txt` |

Qt and VTFLib++ are dynamically linked. The ZIP distribution exposes their DLLs
so users can replace them with compatible builds. The portable launcher extracts
the same distribution into `%LOCALAPPDATA%/VMTStudio/app-v<version>/`; use the ZIP when
working with replacement libraries. VMT-Studio's source and build instructions
are available in [the repository](https://github.com/Darvan76/VMTStudio).
The complete pinned VTFLib source is obtainable with `git submodule update --init
--recursive`; the build patch and application instructions are in
`thirdparty/patches/`. Qt license texts are copied unchanged from the Qt 6.8.2
source `LICENSES/` directory. Qt's own component notices also accompany its source.

The HLMV backend contains SDK-derived source and links SDK libraries. It loads
game engine DLLs from the user's compatible local installation. Retail game DLLs
and game content are not included in release packages. The primitive preview
models are project assets; see `scripts/export_blender_models.py`.

Microsoft Visual C++ runtime components deployed by `windeployqt` remain subject
to Microsoft's applicable redistributable terms. VMT-Studio is not affiliated
with Valve, The Qt Company, or the authors of the bundled libraries.
