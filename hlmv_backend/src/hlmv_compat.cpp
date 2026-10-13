#include <windows.h>

// Compatibility stubs for MSVC / Source SDK linker symbols

// collisionutils.cpp expects: unsigned int const * const g_SIMD_clear_wmask;
static const unsigned int s_clear_wmask[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
extern const unsigned int * const g_SIMD_clear_wmask = s_clear_wmask;

// studio.cpp references HushAsserts from tier0
extern "C" __declspec(dllexport) bool HushAsserts() {
    return false;
}
