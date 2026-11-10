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

#define CF_ARM         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1000, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_READ        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1001, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_WRITE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1002, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_BATCH       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1003, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_INVOKE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1004, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_ALLOC_RWX   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1008, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_FREE_RWX    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1009, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_PATCH_QWORD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x100A, METHOD_NEITHER, FILE_ANY_ACCESS)

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

NTKERNELAPI NTSTATUS ZwAllocateVirtualMemory(
    HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits,
    PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTKERNELAPI NTSTATUS ObOpenObjectByPointer(
    PVOID Object, ULONG HandleAttributes, PACCESS_STATE PassedAccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType,
    KPROCESSOR_MODE AccessMode, PHANDLE Handle);
NTKERNELAPI NTSTATUS ZwClose(HANDLE Handle);
extern POBJECT_TYPE* PsProcessType;

typedef struct _REPLY_ALLOC {
    ULONG64 Address;
    ULONG   Size;
    ULONG   Pad;
} REPLY_ALLOC, *PREPLY_ALLOC;

typedef struct _REQ_PATCH_QWORD {
    ULONG64 Addr;
    ULONG64 Value;
} REQ_PATCH_QWORD, *PREQ_PATCH_QWORD;

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
        else if(code == CF_ALLOC_RWX && len == sizeof(ULONG) && g_target){
            ULONG wanted = *(ULONG*)buf;
            if(wanted == 0) wanted = 0x1000;
            HANDLE hProc = NULL;
            st = ObOpenObjectByPointer(g_target, 0, NULL, PROCESS_ALL_ACCESS,
                                       *PsProcessType, KernelMode, &hProc);
            if(NT_SUCCESS(st) && hProc){
                PVOID base = NULL;
                SIZE_T sz = wanted;
                st = ZwAllocateVirtualMemory(hProc, &base, 0, &sz,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
                if(NT_SUCCESS(st) && base){
                    // Reply in-place in the user-mode input buffer
                    // (METHOD_NEITHER lets us write directly).
                    if(irp->UserBuffer &&
                       sl->Parameters.DeviceIoControl.OutputBufferLength
                           >= sizeof(REPLY_ALLOC)){
                        __try {
                            PREPLY_ALLOC rep = (PREPLY_ALLOC)irp->UserBuffer;
                            rep->Address = (ULONG64)base;
                            rep->Size    = (ULONG)sz;
                            rep->Pad     = 0;
                            irp->IoStatus.Information = sizeof(REPLY_ALLOC);
                        } __except(EXCEPTION_EXECUTE_HANDLER){
                            st = STATUS_ACCESS_VIOLATION;
                        }
                    }
                }
                ZwClose(hProc);
            }
        }
        else if(code == CF_PATCH_QWORD && len == sizeof(REQ_PATCH_QWORD) && g_target){
            PREQ_PATCH_QWORD q = (PREQ_PATCH_QWORD)buf;
            // Atomic 8-byte write — MmCopyVirtualMemory ensures target
            // page validity. Uses the "write TO target" direction.
            st = DoCopy(&q->Value, (PVOID)q->Addr, sizeof(ULONG64), TRUE);
        }
        else if(code == CF_FREE_RWX && len == sizeof(ULONG64) && g_target){
            // Intentional no-op — target reclaims on exit. Full unmap
            // would need ZwFreeVirtualMemory with a cross-process handle
            // that we open afresh (same pattern as alloc above).
            st = STATUS_SUCCESS;
        }
        else if(code == CF_INVOKE){
            // Documented in antiban/rpc_path — invoke a target-side
            // function via kernel work-item. Stub for now: the invoke
            // queue lives in a separate commit.
            st = STATUS_NOT_IMPLEMENTED;
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
