#pragma once

#include "interface.h"

// Returns true if running against Garry's Mod or 2013+ engine with extra IAppSystem slots
bool IsGModEngine();

// Universal wrapper for IAppSystem-derived interfaces to bridge SDK 2013 vtable calls to GMod (+4 slot delta)
void* WrapGModAppSystem(void* pRealObject);

// Specialized wrapper for IMaterialSystem (slots 0..4 delta 0, 5+ delta +4, slot 98 [GetRenderContext] returns wrapped context)
void* WrapGModMaterialSystem(void* pRealObject);

// Specialized wrapper for IMatRenderContext (slots 0..1 delta 0, slots 2..3 dummy stubs, slots 4+ delta -2)
void* WrapGModRenderContext(void* pRealObject);

// Specialized wrapper for IMDLCache (slots 0..4 delta 0, 5..15 delta +4, 16+ delta +5 due to GetHardwareData LOD overload)
void* WrapGModMDLCache(void* pRealObject);

// Factory wrapper used when connecting GMod native modules
void* HLMV_GModFactoryWrapper(const char* pName, int* pReturnCode);

// Fixes GMod filesystem vtable ABI differences (e.g. AddSearchPath 2-args vs 3-args)
void FixGModFileSystemVTable(void* pFileSystem);

// Original factory from CAppSystemGroup
extern CreateInterfaceFn s_pOriginalFactory;
