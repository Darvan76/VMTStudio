#include "hlmv_compat.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <cstdint>

extern void HLMV_Log(const char* fmt, ...);

// Compatibility stubs for MSVC / Source SDK linker symbols

// collisionutils.cpp expects: unsigned int const * const g_SIMD_clear_wmask;
static const unsigned int s_clear_wmask[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
extern const unsigned int * const g_SIMD_clear_wmask = s_clear_wmask;

// studio.cpp references HushAsserts from tier0
extern "C" __declspec(dllexport) bool HushAsserts() {
    return false;
}

// ----------------------------------------------------------------------------
// Static Symbol Emulation for tier0.dll and vstdlib.dll
// Direct definition of __imp__ pointers eliminates unresolved imports from tier0.dll,
// preventing STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) on Garry's Mod while preserving
// full compatibility with standard Source SDK 2013 / TF2 / CSS.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// GMod CommandLine Proxy
// GMod inserted HasParm at slot 4, shifting RemoveParm, AppendParm, ParmValue,
// ParmCount, FindParm, GetParm, SetParm by +1.
// We proxy SDK 2013 calls to GMod methods to allow appframework and mdlviewer
// to parse command line parameters accurately.
// ----------------------------------------------------------------------------
class ICommandLineSDK {
public:
	virtual void		CreateCmdLine( const char *commandline ) = 0; // 0
	virtual void		CreateCmdLine( int argc, char **argv ) = 0;   // 1
	virtual const char	*GetCmdLine( void ) const = 0;                // 2
	virtual	const char	*CheckParm( const char *psz, const char **ppszValue = 0 ) const = 0; // 3
	virtual void		RemoveParm( const char *parm ) = 0;          // 4
	virtual void		AppendParm( const char *pszParm, const char *pszValues ) = 0; // 5
	virtual const char	*ParmValue( const char *psz, const char *pDefaultVal = 0 ) const = 0; // 6
	virtual int			ParmValue( const char *psz, int nDefaultVal ) const = 0; // 7
	virtual float		ParmValue( const char *psz, float flDefaultVal ) const = 0; // 8
	virtual int			ParmCount() const = 0;                       // 9
	virtual int			FindParm( const char *psz ) const = 0;       // 10
	virtual const char* GetParm( int nIndex ) const = 0;             // 11
	virtual void        SetParm( int nIndex, char const *pNewParm ) = 0; // 12
	virtual const char *ParmValueByIndex( int nIndex, const char *pDefaultVal = 0 ) const = 0; // 13
	virtual bool		HasParm( const char *psz ) const = 0;        // 14
	virtual const char **GetParms() const = 0;                       // 15
};

class ICommandLineGMod {
public:
    virtual void CreateCmdLine( const char *commandline ) = 0; // 0
    virtual void CreateCmdLine( int argc, char **argv ) = 0; // 1
    virtual const char *GetCmdLine( void ) const = 0; // 2
    virtual const char *CheckParm( const char *psz, const char **ppszValue = 0 ) const = 0; // 3
    virtual bool HasParm( const char *psz ) const = 0; // 4
    virtual void RemoveParm( const char *parm ) = 0; // 5
    virtual void AppendParm( const char *pszParm, const char *pszValues ) = 0; // 6
    virtual const char *ParmValue( const char *psz, const char *pDefaultVal = 0 ) const = 0; // 7
    virtual int ParmValue( const char *psz, int nDefaultVal ) const = 0; // 8
    virtual float ParmValue( const char *psz, float flDefaultVal ) const = 0; // 9
    virtual int ParmCount() const = 0; // 10
    virtual int FindParm( const char *psz ) const = 0; // 11
    virtual const char* GetParm( int nIndex ) const = 0; // 12
    virtual void SetParm( int nIndex, char const *pNewParm ) = 0; // 13
};

class CCommandLineProxy : public ICommandLineSDK {
public:
    ICommandLineGMod* m_pReal = nullptr;

    virtual void CreateCmdLine(const char* s) override { if (m_pReal) m_pReal->CreateCmdLine(s); }
    virtual void CreateCmdLine(int argc, char** argv) override { if (m_pReal) m_pReal->CreateCmdLine(argc, argv); }
    virtual const char* GetCmdLine() const override { return m_pReal ? m_pReal->GetCmdLine() : ""; }
    virtual const char* CheckParm(const char* psz, const char** ppszVal) const override { return m_pReal ? m_pReal->CheckParm(psz, ppszVal) : nullptr; }
    virtual void RemoveParm(const char* parm) override { if (m_pReal) m_pReal->RemoveParm(parm); }
    virtual void AppendParm(const char* pszParm, const char* pszValues) override { if (m_pReal) m_pReal->AppendParm(pszParm, pszValues); }
    virtual const char* ParmValue(const char* psz, const char* pDefaultVal) const override { return m_pReal ? m_pReal->ParmValue(psz, pDefaultVal) : pDefaultVal; }
    virtual int ParmValue(const char* psz, int nDefaultVal) const override { return m_pReal ? m_pReal->ParmValue(psz, nDefaultVal) : nDefaultVal; }
    virtual float ParmValue(const char* psz, float flDefaultVal) const override { return m_pReal ? m_pReal->ParmValue(psz, flDefaultVal) : flDefaultVal; }
    virtual int ParmCount() const override { return m_pReal ? m_pReal->ParmCount() : 0; }
    virtual int FindParm(const char* psz) const override { return m_pReal ? m_pReal->FindParm(psz) : 0; }
    virtual const char* GetParm(int nIndex) const override { return m_pReal ? m_pReal->GetParm(nIndex) : ""; }
    virtual void SetParm(int nIndex, char const* pNewParm) override { if (m_pReal) m_pReal->SetParm(nIndex, pNewParm); }
    virtual const char* ParmValueByIndex(int nIndex, const char* def) const override { return def; }
    virtual bool HasParm(const char* psz) const override { return m_pReal ? m_pReal->HasParm(psz) : false; }
    virtual const char** GetParms() const override { return nullptr; }
};

static CCommandLineProxy s_gmodCmdLineProxy;
static bool s_bIsGModCmdLine = false;

extern "C" {

typedef void* (*CmdLineFn)();
static CmdLineFn s_pCmdLineFn = nullptr;

void* HLMV_CommandLine()
{
    if (!s_pCmdLineFn) {
        HMODULE hTier0 = GetModuleHandleA("tier0.dll");
        if (!hTier0) {
            hTier0 = LoadLibraryA("tier0.dll");
        }
        if (hTier0) {
            s_pCmdLineFn = (CmdLineFn)GetProcAddress(hTier0, "CommandLine_Tier0");
            if (!s_pCmdLineFn) {
                s_pCmdLineFn = (CmdLineFn)GetProcAddress(hTier0, "CommandLine");
                if (s_pCmdLineFn) {
                    s_bIsGModCmdLine = true;
                }
            }
        }
    }
    if (s_pCmdLineFn) {
        void* pReal = s_pCmdLineFn();
        if (s_bIsGModCmdLine) {
            s_gmodCmdLineProxy.m_pReal = (ICommandLineGMod*)pReal;
            return &s_gmodCmdLineProxy;
        }
        return pReal;
    }
    return nullptr;
}
void* CommandLine_Tier0() { return HLMV_CommandLine(); }
void* (*_imp__CommandLine_Tier0)() = &HLMV_CommandLine;

typedef int (*SpewOutputFunc_t)(int, const char*);
static SpewOutputFunc_t s_SpewFunc = nullptr;
typedef void (*OrigSpewFn)(SpewOutputFunc_t);

void HLMV_SpewOutputFunc(SpewOutputFunc_t func)
{
    s_SpewFunc = func;
    HMODULE hTier0 = GetModuleHandleA("tier0.dll");
    if (hTier0) {
        OrigSpewFn orig = (OrigSpewFn)GetProcAddress(hTier0, "SpewOutputFunc");
        if (orig) {
            orig(func);
        }
    }
}
void SpewOutputFunc(SpewOutputFunc_t func) { HLMV_SpewOutputFunc(func); }
void (*_imp__SpewOutputFunc)(SpewOutputFunc_t) = &HLMV_SpewOutputFunc;

typedef int (*OrigWaitFn)(int, const HANDLE*, bool, unsigned);

int HLMV_ThreadWaitForObjects(int nEvents, const HANDLE* pHandles, bool bWaitAll, unsigned timeout)
{
    HMODULE hTier0 = GetModuleHandleA("tier0.dll");
    if (hTier0) {
        OrigWaitFn orig = (OrigWaitFn)GetProcAddress(hTier0, "ThreadWaitForObjects");
        if (orig) {
            return orig(nEvents, pHandles, bWaitAll, timeout);
        }
    }
    return (int)::WaitForMultipleObjects(nEvents, pHandles, bWaitAll ? TRUE : FALSE, timeout);
}
int ThreadWaitForObjects(int nEvents, const HANDLE* pHandles, bool bWaitAll, unsigned timeout) {
    return HLMV_ThreadWaitForObjects(nEvents, pHandles, bWaitAll, timeout);
}
int (*_imp__ThreadWaitForObjects)(int, const HANDLE*, bool, unsigned) = &HLMV_ThreadWaitForObjects;

// CVar factory wrapper to satisfy VEngineCvar004 requests using GMod's VEngineCvar007
static CreateInterfaceFn s_pRealCVarFactory = nullptr;
static void* CVarFactoryWrapper(const char* pName, int* pReturnCode)
{
    if (!s_pRealCVarFactory) {
        HMODULE hVStd = GetModuleHandleA("vstdlib.dll");
        if (hVStd) {
            typedef CreateInterfaceFn (*OrigFn)();
            OrigFn orig = (OrigFn)GetProcAddress(hVStd, "VStdLib_GetICVarFactory");
            if (orig) s_pRealCVarFactory = orig();
        }
    }
    if (s_pRealCVarFactory) {
        void* p = s_pRealCVarFactory(pName, pReturnCode);
        if (p) return p;
        if (pName && strcmp(pName, "VEngineCvar004") == 0) {
            return s_pRealCVarFactory("VEngineCvar007", pReturnCode);
        }
    }
    return nullptr;
}

CreateInterfaceFn HLMV_GetICVarFactory()
{
    return CVarFactoryWrapper;
}
CreateInterfaceFn (*__imp__VStdLib_GetICVarFactory)() = &HLMV_GetICVarFactory;

} // extern "C"

// ----------------------------------------------------------------------------
// Universal GMod IAppSystem Proxy
// ----------------------------------------------------------------------------
struct GModAppSystemProxy {
    void* pVTable;
    void* pRealObject;
};

static void* s_pUniversalProxyVTable[512] = {0};
static unsigned char* s_pThunkCode = nullptr;

static void InitUniversalProxyVTable()
{
    if (s_pThunkCode) return;

    // Allocate executable memory for 512 thunks
    s_pThunkCode = (unsigned char*)VirtualAlloc(NULL, 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    unsigned char* pCur = s_pThunkCode;
    for (int slot = 0; slot < 512; slot++) {
        s_pUniversalProxyVTable[slot] = pCur;

        // Slots 0..4 (Connect, Disconnect, QueryInterface, Init, Shutdown) match exactly.
        // Slots 5+ have a +4 delta because GMod's IAppSystem introduced 4 new virtual methods.
        int targetSlot = (slot < 5) ? slot : (slot + 4);
        int targetDisp = targetSlot * 4;

        // mov ecx, dword ptr [ecx + 4]  ; switch 'this' from proxy to real object
        *pCur++ = 0x8B;
        *pCur++ = 0x49;
        *pCur++ = 0x04;

        // mov eax, dword ptr [ecx]      ; load real object's vtable
        *pCur++ = 0x8B;
        *pCur++ = 0x01;

        // jmp dword ptr [eax + targetDisp] ; tail-call into real method
        if (targetDisp < 128) {
            *pCur++ = 0xFF;
            *pCur++ = 0x60;
            *pCur++ = (unsigned char)targetDisp;
        } else {
            *pCur++ = 0xFF;
            *pCur++ = 0xA0;
            *(int*)pCur = targetDisp;
            pCur += 4;
        }

        // Align thunks to 16 bytes for instruction cache friendliness
        while (((uintptr_t)pCur % 16) != 0) {
            *pCur++ = 0x90; // nop
        }
    }

    DWORD oldProtect;
    VirtualProtect(s_pThunkCode, 4096 * 2, PAGE_EXECUTE_READ, &oldProtect);
}

void* WrapGModAppSystem(void* pRealObject)
{
    if (!pRealObject) return nullptr;
    InitUniversalProxyVTable();
    GModAppSystemProxy* pProxy = new GModAppSystemProxy();
    pProxy->pVTable = s_pUniversalProxyVTable;
    pProxy->pRealObject = pRealObject;
    HLMV_Log("[HLMV] WrapGModAppSystem: real=%p proxy=%p\n", pRealObject, pProxy);
    return pProxy;
}

static void* s_pMDLCacheProxyVTable[512] = {0};
static unsigned char* s_pMDLThunkCode = nullptr;

static void InitMDLCacheProxyVTable()
{
    if (s_pMDLThunkCode) return;

    s_pMDLThunkCode = (unsigned char*)VirtualAlloc(NULL, 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    unsigned char* pCur = s_pMDLThunkCode;
    for (int slot = 0; slot < 512; slot++) {
        s_pMDLCacheProxyVTable[slot] = pCur;

        int targetSlot;
        if (slot < 5) {
            targetSlot = slot;
        } else if (slot <= 15) {
            targetSlot = slot + 4;
        } else {
            targetSlot = slot + 5;
        }
        int targetDisp = targetSlot * 4;

        // mov ecx, dword ptr [ecx + 4]
        *pCur++ = 0x8B;
        *pCur++ = 0x49;
        *pCur++ = 0x04;

        // mov eax, dword ptr [ecx]
        *pCur++ = 0x8B;
        *pCur++ = 0x01;

        // jmp dword ptr [eax + targetDisp]
        if (targetDisp < 128) {
            *pCur++ = 0xFF;
            *pCur++ = 0x60;
            *pCur++ = (unsigned char)targetDisp;
        } else {
            *pCur++ = 0xFF;
            *pCur++ = 0xA0;
            *(int*)pCur = targetDisp;
            pCur += 4;
        }

        while (((uintptr_t)pCur % 16) != 0) {
            *pCur++ = 0x90;
        }
    }

    DWORD oldProtect;
    VirtualProtect(s_pMDLThunkCode, 4096 * 2, PAGE_EXECUTE_READ, &oldProtect);
}

void* WrapGModMDLCache(void* pRealObject)
{
    if (!pRealObject) return nullptr;
    InitMDLCacheProxyVTable();
    GModAppSystemProxy* pProxy = new GModAppSystemProxy();
    pProxy->pVTable = s_pMDLCacheProxyVTable;
    pProxy->pRealObject = pRealObject;
    return pProxy;
}

// ----------------------------------------------------------------------------
// GMod IMatRenderContext Proxy
// In GMod, BeginRender (slot 2) and EndRender (slot 3) do not exist on IMatRenderContext.
// Slots 0..1 (AddRef, Release) have delta 0.
// Slots 2..3 are no-op stubs (return void).
// Slots 4+ have a constant -2 delta (e.g. SDK slot 31 [LoadIdentity] -> GMod slot 29,
// SDK slot 38 [Viewport] -> GMod slot 36, etc.).
// ----------------------------------------------------------------------------
struct GModRenderContextProxy {
    void* pVTable;
    void* pRealObject;
};

static void* s_pRenderContextProxyVTable[512] = {0};
static unsigned char* s_pRCThunkCode = nullptr;
static GModRenderContextProxy s_gmodRenderContextProxy;

static void InitRenderContextProxyVTable()
{
    if (s_pRCThunkCode) return;

    s_pRCThunkCode = (unsigned char*)VirtualAlloc(NULL, 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    unsigned char* pCur = s_pRCThunkCode;
    for (int slot = 0; slot < 512; slot++) {
        s_pRenderContextProxyVTable[slot] = pCur;

        if (slot == 2 || slot == 3) {
            // BeginRender / EndRender: return void immediately (ret)
            *pCur++ = 0x31; // xor eax, eax
            *pCur++ = 0xC0;
            *pCur++ = 0xC3; // ret
        } else {
            int targetSlot = (slot < 2) ? slot : (slot - 2);
            int targetDisp = targetSlot * 4;

            // mov ecx, dword ptr [ecx + 4]  ; switch 'this' from proxy to real object
            *pCur++ = 0x8B;
            *pCur++ = 0x49;
            *pCur++ = 0x04;

            // mov eax, dword ptr [ecx]      ; load real object's vtable
            *pCur++ = 0x8B;
            *pCur++ = 0x01;

            // jmp dword ptr [eax + targetDisp] ; tail-call into real method
            if (targetDisp < 128) {
                *pCur++ = 0xFF;
                *pCur++ = 0x60;
                *pCur++ = (unsigned char)targetDisp;
            } else {
                *pCur++ = 0xFF;
                *pCur++ = 0xA0;
                *(int*)pCur = targetDisp;
                pCur += 4;
            }
        }

        while (((uintptr_t)pCur % 16) != 0) {
            *pCur++ = 0x90; // nop
        }
    }

    DWORD oldProtect;
    VirtualProtect(s_pRCThunkCode, 4096 * 2, PAGE_EXECUTE_READ, &oldProtect);
}

void* WrapGModRenderContext(void* pRealObject)
{
    if (!pRealObject) return nullptr;
    InitRenderContextProxyVTable();
    s_gmodRenderContextProxy.pVTable = s_pRenderContextProxyVTable;
    s_gmodRenderContextProxy.pRealObject = pRealObject;
    return &s_gmodRenderContextProxy;
}

static void* s_pMatSysProxyVTable[512] = {0};
static unsigned char* s_pMatSysThunkCode = nullptr;

static void* __fastcall MatSys_GetRenderContext_Hook(void* thisptr, void* /*edx*/)
{
    GModAppSystemProxy* pProxy = (GModAppSystemProxy*)thisptr;
    void* pRealMatSys = pProxy->pRealObject;
    void** vt = *(void***)pRealMatSys;

    // GMod slot 102 is GetRenderContext()
    typedef void* (__thiscall *RealGetRenderContextFn)(void*);
    RealGetRenderContextFn realFn = (RealGetRenderContextFn)vt[102];
    void* pRealCtx = realFn(pRealMatSys);

    static bool s_bLoggedRC = false;
    if (!s_bLoggedRC) {
        HLMV_Log("[HLMV] MatSys_GetRenderContext_Hook: realMatSys=%p, realCtx=%p\n", pRealMatSys, pRealCtx);
        s_bLoggedRC = true;
    }

    return WrapGModRenderContext(pRealCtx);
}

static void InitMatSysProxyVTable()
{
    if (s_pMatSysThunkCode) return;

    s_pMatSysThunkCode = (unsigned char*)VirtualAlloc(NULL, 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    unsigned char* pCur = s_pMatSysThunkCode;
    for (int slot = 0; slot < 512; slot++) {
        if (slot == 98) {
            // SDK Slot 98 is GetRenderContext() -> intercept to wrap returned IMatRenderContext
            s_pMatSysProxyVTable[98] = (void*)&MatSys_GetRenderContext_Hook;
            continue;
        }

        s_pMatSysProxyVTable[slot] = pCur;

        // Slots 0..4 (Connect, Disconnect, QueryInterface, Init, Shutdown) match exactly.
        // Slots 5+ have a +4 delta because GMod's IAppSystem introduced 4 new virtual methods at slots 5..8.
        // E.g.: SDK Slot 6 (SetShaderAPI) -> GMod Slot 10 (SetShaderAPI)
        //       SDK Slot 7 (SetAdapter)   -> GMod Slot 11 (SetAdapter)
        //       SDK Slot 98 (GetRenderContext) handled above
        int targetSlot = (slot < 5) ? slot : (slot + 4);
        int targetDisp = targetSlot * 4;

        // mov ecx, dword ptr [ecx + 4]
        *pCur++ = 0x8B;
        *pCur++ = 0x49;
        *pCur++ = 0x04;

        // mov eax, dword ptr [ecx]
        *pCur++ = 0x8B;
        *pCur++ = 0x01;

        // jmp dword ptr [eax + targetDisp]
        if (targetDisp < 128) {
            *pCur++ = 0xFF;
            *pCur++ = 0x60;
            *pCur++ = (unsigned char)targetDisp;
        } else {
            *pCur++ = 0xFF;
            *pCur++ = 0xA0;
            *(int*)pCur = targetDisp;
            pCur += 4;
        }

        while (((uintptr_t)pCur % 16) != 0) {
            *pCur++ = 0x90;
        }
    }

    DWORD oldProtect;
    VirtualProtect(s_pMatSysThunkCode, 4096 * 2, PAGE_EXECUTE_READ, &oldProtect);
}

void* WrapGModMaterialSystem(void* pRealObject)
{
    if (!pRealObject) return nullptr;
    InitMatSysProxyVTable();
    GModAppSystemProxy* pProxy = new GModAppSystemProxy();
    pProxy->pVTable = s_pMatSysProxyVTable;
    pProxy->pRealObject = pRealObject;
    HLMV_Log("[HLMV] WrapGModMaterialSystem: real=%p proxy=%p\n", pRealObject, pProxy);
    return pProxy;
}

bool IsGModEngine()
{
    static int s_isGMod = -1;
    if (s_isGMod != -1) return (s_isGMod == 1);

    HMODULE hTier0 = GetModuleHandleA("tier0.dll");
    if (hTier0 && GetProcAddress(hTier0, "LoggingSystem_Log") != NULL) {
        s_isGMod = 1;
        HLMV_Log("[HLMV] Detected Garry's Mod engine branch (LoggingSystem_Log present)\n");
        return true;
    }
    s_isGMod = 0;
    HLMV_Log("[HLMV] Standard Source SDK 2013 engine detected\n");
    return false;
}

CreateInterfaceFn s_pOriginalFactory = nullptr;

void* HLMV_GModFactoryWrapper(const char* pName, int* pReturnCode)
{
    if (!s_pOriginalFactory) return nullptr;

    // Handle cvar version aliases
    if (pName && strcmp(pName, "VEngineCvar007") == 0) {
        void* p = s_pOriginalFactory("VEngineCvar004", pReturnCode);
        if (p) return p;
    }
    if (pName && strcmp(pName, "VEngineCvar004") == 0) {
        void* p = s_pOriginalFactory("VEngineCvar007", pReturnCode);
        if (p) return p;
    }
    if (pName && strcmp(pName, "MaterialSystemHardwareConfig013") == 0) {
        void* p = s_pOriginalFactory("MaterialSystemHardwareConfig012", pReturnCode);
        if (p) return p;
    }

    void* p = s_pOriginalFactory(pName, pReturnCode);
    if (p) {
        // Native GMod modules (materialsystem, studiorender) query the factory directly
        // and require native GMod vtable layouts. If we wrapped this object, unwrap it here.
        GModAppSystemProxy* proxy = (GModAppSystemProxy*)p;
        if (proxy->pVTable == s_pUniversalProxyVTable ||
            proxy->pVTable == s_pMDLCacheProxyVTable ||
            proxy->pVTable == s_pMatSysProxyVTable ||
            proxy->pVTable == s_pRenderContextProxyVTable) {
            return proxy->pRealObject;
        }
        return p;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// GMod Filesystem ABI Thunks
// ----------------------------------------------------------------------------
typedef void (__thiscall *GModAddSearchPathFn)(void* thisptr, const char* pPath, const char* pathID);
static GModAddSearchPathFn s_pRealGModAddSearchPath = nullptr;

typedef int (__thiscall *GModMountSteamContentFn)(void* thisptr);
static GModMountSteamContentFn s_pRealGModMountSteamContent = nullptr;

typedef bool (__thiscall *GModRemoveSearchPathFn)(void* thisptr);
static GModRemoveSearchPathFn s_pRealGModRemoveSearchPath = nullptr;

class CFSThunks {
public:
    void Hooked_AddSearchPath(const char* pPath, const char* pathID, int addType) {
        HLMV_Log("[HLMV] Hooked_AddSearchPath: path='%s', id='%s', type=%d\n", pPath ? pPath : "null", pathID ? pathID : "null", addType);
        if (s_pRealGModAddSearchPath) {
            s_pRealGModAddSearchPath(this, pPath, pathID);
        }
    }

    int Hooked_MountSteamContent(int nExtraAppId) {
        HLMV_Log("[HLMV] Hooked_MountSteamContent: appid=%d\n", nExtraAppId);
        if (s_pRealGModMountSteamContent) {
            return s_pRealGModMountSteamContent(this);
        }
        return 0;
    }

    bool Hooked_RemoveSearchPath(const char* pPath, const char* pathID) {
        HLMV_Log("[HLMV] Hooked_RemoveSearchPath: path='%s', id='%s'\n", pPath ? pPath : "null", pathID ? pathID : "null");
        if (s_pRealGModRemoveSearchPath) {
            return s_pRealGModRemoveSearchPath(this);
        }
        return true;
    }
};

template<typename TMemberFn>
static void* GetMemberFnPtr(TMemberFn fn) {
    union {
        TMemberFn mfp;
        void* ptr;
    } u;
    u.mfp = fn;
    return u.ptr;
}

void FixGModFileSystemVTable(void* pFileSystem)
{
    if (!pFileSystem || !IsGModEngine()) return;

    static bool s_bAlreadyPatched = false;
    if (s_bAlreadyPatched) return;

    void** vt = *(void***)pFileSystem;
    HLMV_Log("[HLMV] Checking GMod filesystem vtable at %p...\n", vt);

    // Slot 7: AddSearchPath (GMod expects 2 args [ret 8], SDK 2013 caller pushes 3 args [ret 12])
    unsigned char* pSlot7 = (unsigned char*)vt[7];
    if (pSlot7[0] == 0xC2 && pSlot7[1] == 0x08 && pSlot7[2] == 0x00) {
        HLMV_Log("[HLMV] Detected GMod 2-arg AddSearchPath at slot 7. Patching vtable slot 7 to 3-arg adapter...\n");
        DWORD oldProt;
        VirtualProtect(&vt[7], sizeof(void*), PAGE_READWRITE, &oldProt);
        s_pRealGModAddSearchPath = (GModAddSearchPathFn)vt[7];
        vt[7] = GetMemberFnPtr(&CFSThunks::Hooked_AddSearchPath);
        VirtualProtect(&vt[7], sizeof(void*), oldProt, &oldProt);
    }

    // Slot 6: MountSteamContent (GMod expects 0 args [ret 0], SDK 2013 caller pushes 1 arg [ret 4])
    unsigned char* pSlot6 = (unsigned char*)vt[6];
    if (pSlot6[0] == 0xB8 && pSlot6[5] == 0xC3) { // mov eax, 4; ret
        HLMV_Log("[HLMV] Detected GMod 0-arg MountSteamContent at slot 6. Patching vtable slot 6 to 1-arg adapter...\n");
        DWORD oldProt;
        VirtualProtect(&vt[6], sizeof(void*), PAGE_READWRITE, &oldProt);
        s_pRealGModMountSteamContent = (GModMountSteamContentFn)vt[6];
        vt[6] = GetMemberFnPtr(&CFSThunks::Hooked_MountSteamContent);
        VirtualProtect(&vt[6], sizeof(void*), oldProt, &oldProt);
    }

    // Slot 8: RemoveSearchPath (GMod expects 0 args [ret 0], SDK 2013 caller pushes 2 args [ret 8])
    unsigned char* pSlot8 = (unsigned char*)vt[8];
    if (pSlot8[0] == 0xB0 && pSlot8[2] == 0xC3) { // mov al, 1; ret
        HLMV_Log("[HLMV] Detected GMod 0-arg RemoveSearchPath at slot 8. Patching vtable slot 8 to 2-arg adapter...\n");
        DWORD oldProt;
        VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &oldProt);
        s_pRealGModRemoveSearchPath = (GModRemoveSearchPathFn)vt[8];
        vt[8] = GetMemberFnPtr(&CFSThunks::Hooked_RemoveSearchPath);
        VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);
    }

    s_bAlreadyPatched = true;
    HLMV_Log("[HLMV] GMod filesystem vtable patch completed successfully.\n");
}
