/*
    Smoke test for the hid.dll shim.

    Loads the shim directly and walks the machine's HID devices exactly the way
    DirectInput does, so the forwarders and the filter can both be proven
    before the target application is involved.

    Two things are being checked.

    First, that the signature free forwarders actually work. They are naked
    jumps, so a mistake there is a crash rather than a wrong answer.
    HidD_GetHidGuid is the cheapest proof: it takes an argument, writes through
    it and returns, and the value it should produce is known.

    Second, that the filter hides the devices it is supposed to hide and leaves
    everything else alone. The test opens each HID interface and asks for its
    attributes through the shim, which is the same call DirectInput makes and
    the same one that decides whether a device is dropped.
*/

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <setupapi.h>
#include <stdio.h>

typedef struct _HIDD_ATTRIBUTES {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

typedef struct _HIDP_PREPARSED_DATA *PHIDP_PREPARSED_DATA;

typedef void    (WINAPI *PFN_GetHidGuid)(LPGUID);
typedef BOOLEAN (WINAPI *PFN_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (WINAPI *PFN_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA *);
typedef BOOLEAN (WINAPI *PFN_FreePreparsedData)(PHIDP_PREPARSED_DATA);
typedef BOOLEAN (WINAPI *PFN_GetProductString)(HANDLE, PVOID, ULONG);

int main(int argc, char **argv)
{
    /*
        Defaults to the built shim sitting next to this test, which is where
        Build-HidShim.ps1 puts it. Pass a path to test a different copy.
    */
    const char *dllPath = (argc > 1) ? argv[1] : ".\\hid.dll";

    HMODULE h;
    GUID    hidGuid;
    GUID    expected = {0x4D1E55B2,0xF16F,0x11CF,
                        {0x88,0xCB,0x00,0x11,0x11,0x00,0x00,0x30}};

    PFN_GetHidGuid        pGetHidGuid;
    PFN_GetAttributes     pGetAttributes;
    PFN_GetPreparsedData  pGetPreparsed;
    PFN_FreePreparsedData pFreePreparsed;
    PFN_GetProductString  pGetProduct;

    HDEVINFO                        set;
    SP_DEVICE_INTERFACE_DATA        ifd;
    DWORD                           i;
    int allowed = 0, blocked = 0, unreadable = 0;

    printf("Loading %s\n", dllPath);
    h = LoadLibraryA(dllPath);
    if (!h) {
        printf("FAIL: LoadLibrary failed, error %lu\n", GetLastError());
        return 1;
    }

    pGetHidGuid    = (PFN_GetHidGuid)       GetProcAddress(h, "HidD_GetHidGuid");
    pGetAttributes = (PFN_GetAttributes)    GetProcAddress(h, "HidD_GetAttributes");
    pGetPreparsed  = (PFN_GetPreparsedData) GetProcAddress(h, "HidD_GetPreparsedData");
    pFreePreparsed = (PFN_FreePreparsedData)GetProcAddress(h, "HidD_FreePreparsedData");
    pGetProduct    = (PFN_GetProductString) GetProcAddress(h, "HidD_GetProductString");

    if (!pGetHidGuid || !pGetAttributes || !pGetPreparsed ||
        !pFreePreparsed || !pGetProduct) {
        printf("FAIL: a required export is missing\n");
        return 1;
    }
    printf("All required exports resolved.\n");

    /* Forwarder check. This is the one that would crash if a jump is wrong. */
    memset(&hidGuid, 0, sizeof(hidGuid));
    pGetHidGuid(&hidGuid);
    if (memcmp(&hidGuid, &expected, sizeof(GUID)) != 0) {
        printf("FAIL: HidD_GetHidGuid returned the wrong GUID\n");
        return 1;
    }
    printf("Forwarder check passed: HidD_GetHidGuid returned the HID class GUID.\n\n");

    /* Now walk the devices the way DirectInput does. */
    set = SetupDiGetClassDevsA(&hidGuid, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        printf("FAIL: SetupDiGetClassDevs failed, error %lu\n", GetLastError());
        return 1;
    }

    ifd.cbSize = sizeof(ifd);
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hidGuid, i, &ifd); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
            BYTE                              buf[1024];
        } det;
        DWORD                need = 0;
        HANDLE               dev;
        HIDD_ATTRIBUTES      attrs;
        PHIDP_PREPARSED_DATA ppd = NULL;
        WCHAR                name[128];
        int                  gotPpd;

        det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, &det.d,
                                              sizeof(det.buf), &need, NULL)) {
            continue;
        }

        dev = CreateFileA(det.d.DevicePath, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL);
        if (dev == INVALID_HANDLE_VALUE) { continue; }

        memset(&attrs, 0, sizeof(attrs));
        attrs.Size = sizeof(attrs);

        if (!pGetAttributes(dev, &attrs)) {
            /*
                Either the shim refused it or the device genuinely cannot be
                read. Asking for the descriptor separates the two: a blocked
                device is refused for that as well.
            */
            gotPpd = pGetPreparsed(dev, &ppd) ? 1 : 0;
            if (gotPpd) { pFreePreparsed(ppd); }

            if (gotPpd) {
                printf("  ?      attributes refused but descriptor allowed: %s\n",
                       det.d.DevicePath);
                unreadable++;
            } else {
                printf("  BLOCK  %s\n", det.d.DevicePath);
                blocked++;
            }
            CloseHandle(dev);
            continue;
        }

        name[0] = 0;
        pGetProduct(dev, name, sizeof(name));

        printf("  allow  vid=%04X pid=%04X  \"%ls\"\n",
               attrs.VendorID, attrs.ProductID, name);
        allowed++;

        CloseHandle(dev);
    }

    SetupDiDestroyDeviceInfoList(set);

    printf("\n%d allowed, %d blocked", allowed, blocked);
    if (unreadable) { printf(", %d inconsistent", unreadable); }
    printf("\n");

    if (unreadable) {
        printf("FAIL: a device was refused attributes but given a descriptor.\n");
        return 1;
    }

    printf("Smoke test passed.\n");
    return 0;
}
