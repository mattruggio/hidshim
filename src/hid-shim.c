/*
    A hid.dll shim: hides HID devices from a single process.

    Why this exists
    ---------------
    Old DirectInput games tend to break on machines with a lot of HID devices
    attached. Modern peripherals make this common: a single keyboard can
    present a dozen HID collections, and software such as iCUE adds a virtual
    HID bus on top. The motivating case here was NBA Live 2005, which dies a
    few seconds after launch with a corrupted heap. The damage happens while
    DirectInput parses HID report descriptors, and it scales with how many HID
    devices are present.

    Disabling the offending devices in Device Manager avoids it, but that
    reaches every process on the machine rather than just the one that is
    broken, and a crash can leave the machine in that state. This shim exists
    to make the same fix process local.

    The first attempt at a scoped fix was a dinput.dll proxy that filtered the
    device list handed to the application. It failed, and the way it failed is
    what led here. Even with the filter tightened until only the system
    keyboard and mouse reached the application, so it opened no HID device at
    all, the heap was still destroyed. DirectInput enumerates and parses every
    HID device on the machine internally, before it calls back into the
    application even once. A dinput proxy only ever sees that list afterwards.

    So the filtering has to happen below DirectInput, not above it.

    How DirectInput finds devices
    -----------------------------
    dinput.dll imports neither hid.dll nor setupapi.dll. It loads them at
    runtime, which is why their function names appear as strings in the binary
    while the import table mentions only KERNEL32, USER32, WINMM and ADVAPI32.
    The relevant sequence is:

        SetupDiGetClassDevsW(HidGuid)      enumerate HID interfaces
        SetupDiEnumDeviceInterfaces
        SetupDiGetDeviceInterfaceDetailW
        CreateFile(devicePath)             open the device
        HidD_GetAttributes                 read vendor and product ids
        HidD_GetPreparsedData              fetch the report descriptor
        HidP_GetCaps and friends           parse it   <- the damage happens here

    setupapi.dll cannot be intercepted: it is listed in the KnownDLLs registry
    key, so the loader resolves it from the system directory no matter what
    sits in the application directory. hid.dll is not listed, and dinput loads
    it by bare name with LoadLibraryW, so the standard search order applies and
    the application directory is searched first.

    That makes hid.dll the lowest point in this stack that can still be reached
    from the application directory, and it sits in front of the descriptor
    parsing rather than behind it.

    What this does
    --------------
    Forwards all 47 exports to the real hid.dll in the system directory, and
    filters exactly two of them:

      * HidD_GetAttributes reports failure for any device not on the allow
        list, so DirectInput drops it before asking for anything else.
      * HidD_GetPreparsedData reports failure for the same devices, in case
        DirectInput reaches for the descriptor without checking attributes
        first. This is the call that leads directly to the parsing that
        corrupts the heap, so it is the one that really has to hold.

    Everything else is a tail jump to the real function, which is why the
    forwarders carry no signatures: a naked jmp leaves the arguments and the
    stack exactly as the caller built them, and the real function returns
    straight to the caller.

    The policy is an allow list and nothing else: name the devices the target
    should see, normally just a gamepad, and everything else is hidden. The
    target does not lose its keyboard or mouse, because DirectInput reaches
    those through its system keyboard and mouse objects, which do not come from
    this enumeration.

    Unlike disabling devices in Device Manager this changes nothing outside the
    target process. The keyboard keeps working normally everywhere else, there
    is nothing to restore afterwards, and a crash cannot leave devices
    disabled.

    Scope
    -----
    This is a 32 bit DLL for 32 bit processes. The forwarders below jump
    through symbols named _real_*, and that leading underscore is i386 C symbol
    decoration, so the THUNK macro would not assemble for x86-64 as written.
    That is the only thing standing in the way of a 64 bit build.

    Configuration lives in hid-shim.ini next to this DLL.
*/

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/*
    hidsdi.h and hidpi.h are deliberately not included. They declare full
    prototypes for every function below, which the signature free forwarders
    would contradict. Only two types are actually needed here, so they are
    declared locally instead.
*/

typedef struct _HIDD_ATTRIBUTES {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

/* Opaque: only ever passed through. */
typedef struct _HIDP_PREPARSED_DATA *PHIDP_PREPARSED_DATA;

/* ------------------------------------------------------------------ */
/* Configuration and logging. */

#define MAX_RULES 32

typedef struct {
    WORD vid;
    WORD pid;      /* 0 means every product from this vendor */
} Rule;

static struct {
    int  filterEnabled;
    int  logEnabled;
    Rule allow[MAX_RULES];
    int  allowCount;
} g_cfg;

static char             g_logPath[MAX_PATH];
static CRITICAL_SECTION g_lock;
static int              g_lockReady = 0;

static void Log(const char *fmt, ...)
{
    va_list    args;
    FILE      *f;
    SYSTEMTIME st;

    if (!g_cfg.logEnabled || g_logPath[0] == 0) { return; }

    if (g_lockReady) { EnterCriticalSection(&g_lock); }

    f = fopen(g_logPath, "a");
    if (f) {
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
    }

    if (g_lockReady) { LeaveCriticalSection(&g_lock); }
}

/*
    Parse a list of the form "1B1C, 046D:C216" into vendor and product pairs.
    A bare vendor id covers every product from that vendor.
*/
static void ParseRules(const char *text, Rule *out, int *count)
{
    const char *p = text;

    while (*p && *count < MAX_RULES) {
        unsigned vid = 0, pid = 0;
        int      n = 0;

        while (*p == ' ' || *p == ',' || *p == '\t') { p++; }
        if (*p == 0) { break; }

        if (sscanf(p, "%4x:%4x%n", &vid, &pid, &n) == 2) {
            out[*count].vid = (WORD)vid;
            out[*count].pid = (WORD)pid;
            (*count)++;
        }
        else if (sscanf(p, "%4x%n", &vid, &n) == 1) {
            out[*count].vid = (WORD)vid;
            out[*count].pid = 0;
            (*count)++;
        }
        else {
            break;
        }

        while (*p && *p != ',') { p++; }
    }
}

static int RuleMatches(const Rule *rules, int count, WORD vid, WORD pid)
{
    int i;

    for (i = 0; i < count; i++) {
        if (rules[i].vid != vid) { continue; }
        if (rules[i].pid == 0 || rules[i].pid == pid) { return 1; }
    }
    return 0;
}

static void LoadConfig(HMODULE self)
{
    char        ini[MAX_PATH];
    char        buf[512];
    char       *slash;
    const char *localAppData;

    GetModuleFileNameA(self, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) { slash[1] = 0; }
    strncat(ini, "hid-shim.ini", MAX_PATH - strlen(ini) - 1);

    g_cfg.filterEnabled = GetPrivateProfileIntA("Filter", "Enabled", 1, ini);
    g_cfg.logEnabled    = GetPrivateProfileIntA("Log",    "Enabled", 1, ini);

    /*
        Every device the target is allowed to see. Empty by default, which
        hides everything: safe, but your gamepad will not work until you name
        it.
    */
    buf[0] = 0;
    GetPrivateProfileStringA("Filter", "AllowDevices", "",
                             buf, sizeof(buf), ini);
    ParseRules(buf, g_cfg.allow, &g_cfg.allowCount);

    /*
        Where the log goes. An explicit Path in the ini wins, which matters
        when one machine runs more than one shimmed application and you want
        their logs kept apart.

        The default is under LOCALAPPDATA rather than next to this DLL,
        because the DLL usually lands somewhere under Program Files where the
        target process cannot write.
    */
    buf[0] = 0;
    GetPrivateProfileStringA("Log", "Path", "", buf, sizeof(buf), ini);

    if (buf[0]) {
        _snprintf(g_logPath, MAX_PATH, "%s", buf);
        g_logPath[MAX_PATH - 1] = 0;
    }
    else {
        localAppData = getenv("LOCALAPPDATA");
        if (localAppData && *localAppData) {
            _snprintf(g_logPath, MAX_PATH, "%s\\hidshim", localAppData);
            g_logPath[MAX_PATH - 1] = 0;
            CreateDirectoryA(g_logPath, NULL);
            _snprintf(g_logPath, MAX_PATH, "%s\\hidshim\\hid-shim.log",
                      localAppData);
            g_logPath[MAX_PATH - 1] = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*
    Pointers into the real hid.dll.

    These have external linkage on purpose. The only reads happen from inline
    assembly, which the optimiser cannot see, so a static would look write only
    and could be discarded.
*/

#define REAL(name) FARPROC real_##name = NULL;

REAL(HidD_FlushQueue)
REAL(HidD_FreePreparsedData)
REAL(HidD_GetConfiguration)
REAL(HidD_GetFeature)
REAL(HidD_GetHidGuid)
REAL(HidD_GetIndexedString)
REAL(HidD_GetInputReport)
REAL(HidD_GetManufacturerString)
REAL(HidD_GetMsGenreDescriptor)
REAL(HidD_GetNumInputBuffers)
REAL(HidD_GetPhysicalDescriptor)
REAL(HidD_GetProductString)
REAL(HidD_GetSerialNumberString)
REAL(HidD_Hello)
REAL(HidD_SetConfiguration)
REAL(HidD_SetFeature)
REAL(HidD_SetNumInputBuffers)
REAL(HidD_SetOutputReport)
REAL(HidP_GetButtonArray)
REAL(HidP_GetButtonCaps)
REAL(HidP_GetCaps)
REAL(HidP_GetData)
REAL(HidP_GetExtendedAttributes)
REAL(HidP_GetLinkCollectionNodes)
REAL(HidP_GetScaledUsageValue)
REAL(HidP_GetSpecificButtonCaps)
REAL(HidP_GetSpecificValueCaps)
REAL(HidP_GetUsageValue)
REAL(HidP_GetUsageValueArray)
REAL(HidP_GetUsages)
REAL(HidP_GetUsagesEx)
REAL(HidP_GetValueCaps)
REAL(HidP_GetVersionInternal)
REAL(HidP_InitializeReportForID)
REAL(HidP_MaxDataListLength)
REAL(HidP_MaxUsageListLength)
REAL(HidP_SetButtonArray)
REAL(HidP_SetData)
REAL(HidP_SetScaledUsageValue)
REAL(HidP_SetUsageValue)
REAL(HidP_SetUsageValueArray)
REAL(HidP_SetUsages)
REAL(HidP_TranslateUsagesToI8042ScanCodes)
REAL(HidP_UnsetUsages)
REAL(HidP_UsageListDifference)

/* The two that are filtered need real signatures rather than a blind jump. */
typedef BOOLEAN (NTAPI *PFN_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (NTAPI *PFN_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA *);

static PFN_GetAttributes    t_GetAttributes    = NULL;
static PFN_GetPreparsedData t_GetPreparsedData = NULL;

static HMODULE g_realHid = NULL;

/* ------------------------------------------------------------------ */
/*
    Forwarders.

    A naked function emits no prologue or epilogue, so the jump lands in the
    real function with the caller's stack frame untouched. The real function
    consumes the arguments, cleans up and returns directly to the caller, which
    is why these need no parameter lists and cannot get a signature wrong.
*/

#define THUNK(name)                                            \
    __declspec(dllexport) __attribute__((naked))               \
    void name(void)                                            \
    {                                                          \
        __asm__ __volatile__("jmp *_real_" #name);             \
    }

THUNK(HidD_FlushQueue)
THUNK(HidD_FreePreparsedData)
THUNK(HidD_GetConfiguration)
THUNK(HidD_GetFeature)
THUNK(HidD_GetHidGuid)
THUNK(HidD_GetIndexedString)
THUNK(HidD_GetInputReport)
THUNK(HidD_GetManufacturerString)
THUNK(HidD_GetMsGenreDescriptor)
THUNK(HidD_GetNumInputBuffers)
THUNK(HidD_GetPhysicalDescriptor)
THUNK(HidD_GetProductString)
THUNK(HidD_GetSerialNumberString)
THUNK(HidD_Hello)
THUNK(HidD_SetConfiguration)
THUNK(HidD_SetFeature)
THUNK(HidD_SetNumInputBuffers)
THUNK(HidD_SetOutputReport)
THUNK(HidP_GetButtonArray)
THUNK(HidP_GetButtonCaps)
THUNK(HidP_GetCaps)
THUNK(HidP_GetData)
THUNK(HidP_GetExtendedAttributes)
THUNK(HidP_GetLinkCollectionNodes)
THUNK(HidP_GetScaledUsageValue)
THUNK(HidP_GetSpecificButtonCaps)
THUNK(HidP_GetSpecificValueCaps)
THUNK(HidP_GetUsageValue)
THUNK(HidP_GetUsageValueArray)
THUNK(HidP_GetUsages)
THUNK(HidP_GetUsagesEx)
THUNK(HidP_GetValueCaps)
THUNK(HidP_GetVersionInternal)
THUNK(HidP_InitializeReportForID)
THUNK(HidP_MaxDataListLength)
THUNK(HidP_MaxUsageListLength)
THUNK(HidP_SetButtonArray)
THUNK(HidP_SetData)
THUNK(HidP_SetScaledUsageValue)
THUNK(HidP_SetUsageValue)
THUNK(HidP_SetUsageValueArray)
THUNK(HidP_SetUsages)
THUNK(HidP_TranslateUsagesToI8042ScanCodes)
THUNK(HidP_UnsetUsages)
THUNK(HidP_UsageListDifference)

/* ------------------------------------------------------------------ */
/* The filter. */

/*
    The policy, in one place so both filtered exports always agree.

    Nothing reaches DirectInput unless it is named in AllowDevices. That
    requires knowing only which device you actually want, normally just your
    gamepad, so it transfers to another machine without having to identify the
    culprit first.

    A block list was supported here too, and was the default. It was removed
    because it cannot express the thing that has to be expressed: a rule names
    a vendor id, and a virtual HID bus such as the one iCUE installs has no
    vendor id at all, so no block list can hide it. It also demanded the harder
    piece of knowledge, asking which device is at fault rather than which one
    you want.

    An empty AllowDevices is therefore the safe default rather than an
    unconfigured one: every HID device is hidden, which cannot crash. The
    target still gets its keyboard and mouse, which come from DirectInput's
    system keyboard and mouse objects and never pass through this filter.
*/
static int PolicyBlocks(WORD vid, WORD pid)
{
    if (!g_cfg.filterEnabled) { return 0; }

    return !RuleMatches(g_cfg.allow, g_cfg.allowCount, vid, pid);
}

/*
    Decided from the device handle every time rather than from a remembered
    list, so the answer does not depend on the order DirectInput happens to
    call things in.
*/
static int DeviceBlocked(HANDLE h, WORD *pvid, WORD *ppid)
{
    HIDD_ATTRIBUTES a;

    if (pvid) { *pvid = 0; }
    if (ppid) { *ppid = 0; }

    if (!g_cfg.filterEnabled || !t_GetAttributes) { return 0; }

    memset(&a, 0, sizeof(a));
    a.Size = sizeof(a);
    if (!t_GetAttributes(h, &a)) { return 0; }

    if (pvid) { *pvid = a.VendorID; }
    if (ppid) { *ppid = a.ProductID; }

    return PolicyBlocks(a.VendorID, a.ProductID);
}

__declspec(dllexport)
BOOLEAN NTAPI HidD_GetAttributes(HANDLE HidDeviceObject,
                                 PHIDD_ATTRIBUTES Attributes)
{
    BOOLEAN ok;

    if (!t_GetAttributes) { return FALSE; }

    ok = t_GetAttributes(HidDeviceObject, Attributes);
    if (!ok || !Attributes) { return ok; }

    if (PolicyBlocks(Attributes->VendorID, Attributes->ProductID)) {

        Log("BLOCK GetAttributes  vid=%04X pid=%04X",
            Attributes->VendorID, Attributes->ProductID);
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    Log("allow GetAttributes  vid=%04X pid=%04X",
        Attributes->VendorID, Attributes->ProductID);
    return ok;
}

__declspec(dllexport)
BOOLEAN NTAPI HidD_GetPreparsedData(HANDLE HidDeviceObject,
                                    PHIDP_PREPARSED_DATA *PreparsedData)
{
    WORD vid = 0, pid = 0;

    if (!t_GetPreparsedData) { return FALSE; }

    /*
        The important one. Everything that corrupts the heap happens after
        DirectInput gets a report descriptor, so refusing here keeps the parser
        away from the device entirely.
    */
    if (DeviceBlocked(HidDeviceObject, &vid, &pid)) {
        Log("BLOCK GetPreparsedData  vid=%04X pid=%04X", vid, pid);
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    Log("allow GetPreparsedData  vid=%04X pid=%04X", vid, pid);
    return t_GetPreparsedData(HidDeviceObject, PreparsedData);
}

/* ------------------------------------------------------------------ */
/* Startup. */

static int LoadReal(void)
{
    char path[MAX_PATH];
    UINT n;

    n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 10) { return 0; }
    strncat(path, "\\hid.dll", MAX_PATH - strlen(path) - 1);

    /*
        Loaded by full path. This DLL has the same file name, so a bare name
        would find this one again.
    */
    g_realHid = LoadLibraryA(path);
    if (!g_realHid) { return 0; }

#define RESOLVE(name) real_##name = GetProcAddress(g_realHid, #name);

    RESOLVE(HidD_FlushQueue)
    RESOLVE(HidD_FreePreparsedData)
    RESOLVE(HidD_GetConfiguration)
    RESOLVE(HidD_GetFeature)
    RESOLVE(HidD_GetHidGuid)
    RESOLVE(HidD_GetIndexedString)
    RESOLVE(HidD_GetInputReport)
    RESOLVE(HidD_GetManufacturerString)
    RESOLVE(HidD_GetMsGenreDescriptor)
    RESOLVE(HidD_GetNumInputBuffers)
    RESOLVE(HidD_GetPhysicalDescriptor)
    RESOLVE(HidD_GetProductString)
    RESOLVE(HidD_GetSerialNumberString)
    RESOLVE(HidD_Hello)
    RESOLVE(HidD_SetConfiguration)
    RESOLVE(HidD_SetFeature)
    RESOLVE(HidD_SetNumInputBuffers)
    RESOLVE(HidD_SetOutputReport)
    RESOLVE(HidP_GetButtonArray)
    RESOLVE(HidP_GetButtonCaps)
    RESOLVE(HidP_GetCaps)
    RESOLVE(HidP_GetData)
    RESOLVE(HidP_GetExtendedAttributes)
    RESOLVE(HidP_GetLinkCollectionNodes)
    RESOLVE(HidP_GetScaledUsageValue)
    RESOLVE(HidP_GetSpecificButtonCaps)
    RESOLVE(HidP_GetSpecificValueCaps)
    RESOLVE(HidP_GetUsageValue)
    RESOLVE(HidP_GetUsageValueArray)
    RESOLVE(HidP_GetUsages)
    RESOLVE(HidP_GetUsagesEx)
    RESOLVE(HidP_GetValueCaps)
    RESOLVE(HidP_GetVersionInternal)
    RESOLVE(HidP_InitializeReportForID)
    RESOLVE(HidP_MaxDataListLength)
    RESOLVE(HidP_MaxUsageListLength)
    RESOLVE(HidP_SetButtonArray)
    RESOLVE(HidP_SetData)
    RESOLVE(HidP_SetScaledUsageValue)
    RESOLVE(HidP_SetUsageValue)
    RESOLVE(HidP_SetUsageValueArray)
    RESOLVE(HidP_SetUsages)
    RESOLVE(HidP_TranslateUsagesToI8042ScanCodes)
    RESOLVE(HidP_UnsetUsages)
    RESOLVE(HidP_UsageListDifference)

    t_GetAttributes =
        (PFN_GetAttributes)GetProcAddress(g_realHid, "HidD_GetAttributes");
    t_GetPreparsedData =
        (PFN_GetPreparsedData)GetProcAddress(g_realHid, "HidD_GetPreparsedData");

    return (t_GetAttributes != NULL && t_GetPreparsedData != NULL);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    int i;

    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);

        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;

        LoadConfig(hinst);

        /*
            Resolved here rather than lazily because the forwarders are naked
            jumps with nowhere to put an initialisation check. The real hid.dll
            depends only on ntdll and kernel32, so loading it under the loader
            lock is safe.
        */
        if (!LoadReal()) {
            Log("=== hid shim failed to load the real hid.dll, expect trouble ===");
            return TRUE;
        }

        Log("=== hid shim loaded, filter %s ===",
            g_cfg.filterEnabled ? "on" : "off");
        Log("  every device not listed below is hidden from the target");
        if (g_cfg.allowCount == 0) {
            Log("  AllowDevices is empty: every HID device is hidden");
        }
        for (i = 0; i < g_cfg.allowCount; i++) {
            if (g_cfg.allow[i].pid) {
                Log("  allow %04X:%04X", g_cfg.allow[i].vid, g_cfg.allow[i].pid);
            } else {
                Log("  allow %04X (any product)", g_cfg.allow[i].vid);
            }
        }
    }

    return TRUE;
}
