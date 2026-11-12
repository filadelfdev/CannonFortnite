// CfSpoof — kernel driver stub for HWID spoofing.
//
// Overwrites the in-memory views of SMBIOS, disk IDENTIFY and NDIS
// permanent MAC that user-mode game anti-cheats query. Does NOT touch
// burned ROMs — it hooks the read paths (NtEnumerateSystemFirmwareTable
// for SMBIOS, IOCTL_STORAGE_QUERY_PROPERTY for disk, OID_802_3_PERMANENT_ADDRESS
// for NICs) and replaces the returned buffer in-flight.
//
// Build with WDK 10. Not signed here — ride your own cert, or test on
// a VM with signing enforcement off (bcdedit /set testsigning on).
//
// Entry: DriverEntry → create \Device\CfSpoof + symlink \DosDevices\CfSpoof
// IOCTL: 0x800  (payload: 64 smbios + 64 disk + 6 mac).
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>

#define CF_IOCTL_APPLY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct _CF_PAYLOAD {
    CHAR SmbiosUuid[64];
    CHAR DiskSerial [64];
    UCHAR Mac[6];
} CF_PAYLOAD, *PCF_PAYLOAD;

static CF_PAYLOAD g_pay;
static BOOLEAN    g_armed = FALSE;

// --- SMBIOS view patch -------------------------------------------------
// We intercept the type-1 (system info) table pulled by NtEnumerateSystemFirmwareTable.
// The anti-cheat driver reads it via ZwQuerySystemInformation(SystemFirmwareTable).
// We flip the UUID field inside the type-1 row during a copy-out hook.

// Convert the 32-char hex SMBIOS UUID string the user-mode caller
// provides into its 16-byte binary form expected by the SMBIOS spec.
// Returns FALSE if the input isn't a valid 32-hex-digit string.
static BOOLEAN HexStringToUuid16(const CHAR* s, UCHAR Out[16]){
    SIZE_T written = 0;
    for(SIZE_T i = 0; i < 64 && written < 16; ++i){
        CHAR a = s[i];
        if(a == 0) break;
        CHAR b = s[i+1];
        if(b == 0) return FALSE;
        UCHAR hi, lo;
        if      (a >= '0' && a <= '9') hi = (UCHAR)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (UCHAR)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') hi = (UCHAR)(a - 'A' + 10);
        else continue;   // skip separators like '-'
        if      (b >= '0' && b <= '9') lo = (UCHAR)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (UCHAR)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') lo = (UCHAR)(b - 'A' + 10);
        else continue;
        Out[written++] = (UCHAR)((hi << 4) | lo);
        ++i;   // consumed two chars
    }
    return (BOOLEAN)(written == 16);
}

static VOID PatchSmbios(PUCHAR Table, SIZE_T Len){
    if(!g_armed) return;
    UCHAR uuidBin[16];
    if(!HexStringToUuid16(g_pay.SmbiosUuid, uuidBin)) return;

    for(SIZE_T i=0; i+0x1C < Len; ){
        UCHAR type = Table[i];
        UCHAR hdr  = Table[i+1];
        if(type == 1 && hdr >= 0x1A){
            // SMBIOS type-1 UUID is a 16-byte binary field at +0x08,
            // NOT a 32-character hex string. The old code copied the
            // ASCII bytes of the hex string there, which looked random
            // but was invalid UUID data.
            RtlCopyMemory(Table + i + 0x08, uuidBin, 16);
        }
        // skip header + strings (null-null terminated)
        SIZE_T j = i + hdr;
        while(j+1 < Len && !(Table[j]==0 && Table[j+1]==0)) ++j;
        i = j + 2;
    }
}

// --- Disk serial patch -------------------------------------------------
static VOID PatchDiskSerial(PUCHAR Buf, SIZE_T Len){
    if(!g_armed || Len < 0x40) return;
    // STORAGE_DEVICE_DESCRIPTOR layout — SerialNumberOffset at +0x18.
    ULONG off = *(ULONG*)(Buf + 0x18);
    if(off && off < Len){
        PCHAR dst = (PCHAR)(Buf + off);
        SIZE_T room = Len - off;
        if(room > sizeof(g_pay.DiskSerial)) room = sizeof(g_pay.DiskSerial);
        RtlStringCbCopyA(dst, room, g_pay.DiskSerial);
    }
}

// --- NDIS permanent MAC patch -----------------------------------------
// Filter driver: on OID_802_3_PERMANENT_ADDRESS completion, rewrite the 6-byte
// response. Wiring omitted in this stub — see the WDK NDIS filter sample.

static NTSTATUS CfCreate(PDEVICE_OBJECT dev, PIRP irp){
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
static NTSTATUS CfClose(PDEVICE_OBJECT dev, PIRP irp){
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
static NTSTATUS CfControl(PDEVICE_OBJECT dev, PIRP irp){
    UNREFERENCED_PARAMETER(dev);
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    if(sl->Parameters.DeviceIoControl.IoControlCode == CF_IOCTL_APPLY){
        if(sl->Parameters.DeviceIoControl.InputBufferLength >= sizeof(CF_PAYLOAD)){
            RtlCopyMemory(&g_pay, irp->AssociatedIrp.SystemBuffer, sizeof(CF_PAYLOAD));
            g_armed = TRUE;
            st = STATUS_SUCCESS;
        }
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID CfUnload(PDRIVER_OBJECT drv){
    UNICODE_STRING sym = RTL_CONSTANT_STRING(L"\\DosDevices\\CfSpoof");
    IoDeleteSymbolicLink(&sym);
    if(drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg){
    UNREFERENCED_PARAMETER(reg);

    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\CfSpoof");
    UNICODE_STRING sym  = RTL_CONSTANT_STRING(L"\\DosDevices\\CfSpoof");

    PDEVICE_OBJECT dev = NULL;
    NTSTATUS st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if(!NT_SUCCESS(st)) return st;

    st = IoCreateSymbolicLink(&sym, &name);
    if(!NT_SUCCESS(st)){ IoDeleteDevice(dev); return st; }

    drv->MajorFunction[IRP_MJ_CREATE]         = CfCreate;
    drv->MajorFunction[IRP_MJ_CLOSE]          = CfClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CfControl;
    drv->DriverUnload = CfUnload;

    // PatchSmbios / PatchDiskSerial get wired into the real syscall hook
    // or NDIS filter path per the build target — left as a seam so the
    // loader picks the hook style (KiServiceTable splice vs. filter
    // driver attach) that fits the host anti-cheat posture.
    (void)PatchSmbios; (void)PatchDiskSerial;

    return STATUS_SUCCESS;
}
