// CfRead — kernel memory read/write bridge.
//
// User-mode CfRead handle sends IOCTLs. The driver resolves the target
// EPROCESS by image-hash walking PsActiveProcessHead, then does
// MmCopyVirtualMemory between the target address space and a user-mode
// buffer the caller supplies. No OpenProcess, no NtReadVirtualMemory,
// no VM-read thread origin in the target's syscall trace.
//
// Entry: DriverEntry → \Device\CfRead + \DosDevices\CfRead
// IOCTLs:
//   0x81001000 ARM      <uint64 image-hash>      → resolve + pin EPROCESS
//   0x81001004 READ     <ReqWire{src,dst,size}>  → MmCopyVM target → user
//   0x81001008 WRITE    <ReqWire{src,dst,size}>  → MmCopyVM user → target
//   0x8100100C BATCH    <ReqWire[]>              → coalesced read loop
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>

#define CF_ARM    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1000, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1001, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_WRITE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1002, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_BATCH  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1003, METHOD_NEITHER, FILE_ANY_ACCESS)

typedef struct _REQ_WIRE {
    ULONG64 Src;
    ULONG64 Dst;
    ULONG   Size;
    ULONG   Pad;
} REQ_WIRE, *PREQ_WIRE;

NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize);

NTKERNELAPI PCHAR PsGetProcessImageFileName(PEPROCESS);
NTKERNELAPI PPEB  PsGetProcessPeb(PEPROCESS);

static PEPROCESS g_target = NULL;

static ULONG64 Fnv1a(const CHAR* s){
    ULONG64 h = 0xcbf29ce484222325ULL;
    while(*s){
        CHAR c = (*s >= 'A' && *s <= 'Z') ? (CHAR)(*s + 32) : *s;
        h ^= (UCHAR)c; h *= 0x100000001b3ULL;
        ++s;
    }
    return h;
}

static NTSTATUS ResolveTarget(ULONG64 hash){
    // Walk the kernel process list via PsLookupProcessByProcessId with
    // short PIDs up to a cap — safer than walking PsActiveProcessHead
    // without a public offset.
    for(ULONG pid = 4; pid < 0x30000; pid += 4){
        PEPROCESS proc = NULL;
        if(!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc))) continue;
        PCHAR name = PsGetProcessImageFileName(proc);
        if(name && Fnv1a(name) == hash){
            if(g_target) ObDereferenceObject(g_target);
            g_target = proc;
            return STATUS_SUCCESS;
        }
        ObDereferenceObject(proc);
    }
    return STATUS_NOT_FOUND;
}

static NTSTATUS DoCopy(PVOID src, PVOID dst, SIZE_T size, BOOLEAN toTarget){
    if(!g_target || !size || size > 0x200000) return STATUS_INVALID_PARAMETER;
    SIZE_T done = 0;
    PEPROCESS from = toTarget ? PsGetCurrentProcess() : g_target;
    PEPROCESS to   = toTarget ? g_target             : PsGetCurrentProcess();
    return MmCopyVirtualMemory(from, src, to, dst, size, KernelMode, &done);
}

static NTSTATUS CfCreate(PDEVICE_OBJECT d, PIRP i){ UNREFERENCED_PARAMETER(d);
    i->IoStatus.Status = STATUS_SUCCESS; i->IoStatus.Information = 0;
    IoCompleteRequest(i, IO_NO_INCREMENT); return STATUS_SUCCESS;
}
static NTSTATUS CfClose (PDEVICE_OBJECT d, PIRP i){ UNREFERENCED_PARAMETER(d);
    i->IoStatus.Status = STATUS_SUCCESS; i->IoStatus.Information = 0;
    IoCompleteRequest(i, IO_NO_INCREMENT); return STATUS_SUCCESS;
}

static NTSTATUS CfControl(PDEVICE_OBJECT dev, PIRP irp){
    UNREFERENCED_PARAMETER(dev);
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
    PVOID buf  = sl->Parameters.DeviceIoControl.Type3InputBuffer;
    ULONG len  = sl->Parameters.DeviceIoControl.InputBufferLength;

    __try {
        if(code == CF_ARM && len == sizeof(ULONG64)){
            st = ResolveTarget(*(ULONG64*)buf);
        }
        else if((code == CF_READ || code == CF_WRITE) && len == sizeof(REQ_WIRE)){
            PREQ_WIRE r = (PREQ_WIRE)buf;
            st = DoCopy((PVOID)r->Src, (PVOID)r->Dst, r->Size, code == CF_WRITE);
        }
        else if(code == CF_BATCH && len % sizeof(REQ_WIRE) == 0){
            PREQ_WIRE r = (PREQ_WIRE)buf;
            ULONG n = len / sizeof(REQ_WIRE);
            st = STATUS_SUCCESS;
            for(ULONG i=0;i<n;++i){
                NTSTATUS one = DoCopy((PVOID)r[i].Src, (PVOID)r[i].Dst, r[i].Size, FALSE);
                if(!NT_SUCCESS(one)) st = one;    // best effort
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER){ st = STATUS_ACCESS_VIOLATION; }

    irp->IoStatus.Status = st;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID CfUnload(PDRIVER_OBJECT drv){
    UNICODE_STRING sym = RTL_CONSTANT_STRING(L"\\DosDevices\\CfRead");
    IoDeleteSymbolicLink(&sym);
    if(drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
    if(g_target){ ObDereferenceObject(g_target); g_target = NULL; }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg){
    UNREFERENCED_PARAMETER(reg);
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\CfRead");
    UNICODE_STRING sym  = RTL_CONSTANT_STRING(L"\\DosDevices\\CfRead");
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if(!NT_SUCCESS(st)) return st;
    st = IoCreateSymbolicLink(&sym, &name);
    if(!NT_SUCCESS(st)){ IoDeleteDevice(dev); return st; }
    drv->MajorFunction[IRP_MJ_CREATE]         = CfCreate;
    drv->MajorFunction[IRP_MJ_CLOSE]          = CfClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CfControl;
    drv->DriverUnload = CfUnload;
    return STATUS_SUCCESS;
}
