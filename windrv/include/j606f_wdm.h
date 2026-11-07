// Minimal WDM + HID minidriver definitions for J606F ARM64 kernel drivers (mingw's ddk/wdm.h has no ARM64 support).
// Layouts are the 64-bit Windows ones (x64 and ARM64 share them); offsets were checked against mingw ddk/wdm.h built for x86_64
// (2026-11-04): IRP.IoStatus 0x30, Cancel 0x44, CancelIrql 0x45, CancelRoutine 0x68, UserBuffer 0x70, Tail.Overlay.ListEntry 0xa8,
// CurrentStackLocation 0xb8, sizeof(IRP) 0xd0; IO_STACK_LOCATION Parameters 8, IoControlCode 0x18, Type3InputBuffer 0x20,
// CompletionRoutine 0x38, sizeof 0x48; DEVICE_OBJECT Flags 0x30, DeviceExtension 0x40; DRIVER_OBJECT DriverExtension 0x30,
// MajorFunction 0x70; DRIVER_EXTENSION AddDevice 8; sizeof(KEVENT) 0x18. HID structures: hidport.h / hidclass.h (WDK).
#pragma once
#include "j606f_nt.h"

typedef UCHAR KIRQL;
typedef CHAR CCHAR;
typedef int64_t LONG_PTR;
typedef ULONG_PTR KSPIN_LOCK, *PKSPIN_LOCK;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
typedef struct _KEVENT { ULONG_PTR Opaque[3]; } KEVENT, *PKEVENT;

typedef struct _DEVICE_OBJECT {
    CSHORT Type;
    USHORT Size;
    LONG ReferenceCount;
    struct _DRIVER_OBJECT *DriverObject;
    struct _DEVICE_OBJECT *NextDevice;
    struct _DEVICE_OBJECT *AttachedDevice;
    struct _IRP *CurrentIrp;
    PVOID Timer;
    ULONG Flags;
    ULONG Characteristics;
    PVOID Vpb;
    PVOID DeviceExtension;
    ULONG DeviceType;
    CCHAR StackSize;
    // the rest is never touched (the object is allocated by the I/O manager / hidclass)
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);
typedef struct _DRIVER_EXTENSION {
    PDRIVER_OBJECT DriverObject;
    PDRIVER_ADD_DEVICE AddDevice;
    ULONG Count;
    UNICODE_STRING ServiceKeyName;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

struct _IO_STACK_LOCATION;
typedef VOID (*PDRIVER_CANCEL)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp);
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT DeviceObject, struct _IRP *Irp, PVOID Context);

typedef struct _IRP {
    CSHORT Type;
    USHORT Size;
    USHORT AllocationProcessorNumber;
    USHORT Reserved;
    PVOID MdlAddress;                               // 0x08
    ULONG Flags;                                    // 0x10
    union { struct _IRP *MasterIrp; LONG IrpCount; PVOID SystemBuffer; } AssociatedIrp;  // 0x18
    LIST_ENTRY ThreadListEntry;                     // 0x20
    IO_STATUS_BLOCK IoStatus;                       // 0x30
    CHAR RequestorMode;                             // 0x40
    BOOLEAN PendingReturned;
    CHAR StackCount;
    CHAR CurrentLocation;
    BOOLEAN Cancel;                                 // 0x44
    KIRQL CancelIrql;
    CCHAR ApcEnvironment;
    UCHAR AllocationFlags;
    PIO_STATUS_BLOCK UserIosb;                      // 0x48
    PVOID UserEvent;                                // 0x50
    union { struct { PVOID UserApcRoutine; PVOID UserApcContext; } AsynchronousParameters; LARGE_INTEGER AllocationSize; } Overlay;  // 0x58
    PDRIVER_CANCEL CancelRoutine;                   // 0x68
    PVOID UserBuffer;                               // 0x70
    union {
        struct {
            PVOID DriverContext[4];                 // 0x78 (union with KDEVICE_QUEUE_ENTRY, 0x18)
            PVOID Thread;                           // 0x98
            PCHAR AuxiliaryBuffer;                  // 0xa0
            LIST_ENTRY ListEntry;                   // 0xa8
            struct _IO_STACK_LOCATION *CurrentStackLocation;   // 0xb8
            PVOID OriginalFileObject;               // 0xc0
            PVOID IrpExtension;                     // 0xc8
        } Overlay;
    } Tail;
} IRP, *PIRP;

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union {
        struct {
            ULONG OutputBufferLength; ULONG _p0;
            ULONG InputBufferLength; ULONG _p1;
            ULONG IoControlCode; ULONG _p2;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct { ULONG SystemContext; ULONG _p0; ULONG Type; ULONG _p1; ULONG State; ULONG _p2; ULONG ShutdownType; } Power;
        struct { PVOID Argument1, Argument2, Argument3, Argument4; } Others;
    } Parameters;                                   // 0x08
    PDEVICE_OBJECT DeviceObject;                    // 0x28
    PVOID FileObject;                               // 0x30
    PIO_COMPLETION_ROUTINE CompletionRoutine;       // 0x38
    PVOID Context;                                  // 0x40
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

_Static_assert(__builtin_offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus");
_Static_assert(__builtin_offsetof(IRP, Cancel) == 0x44, "IRP.Cancel");
_Static_assert(__builtin_offsetof(IRP, CancelRoutine) == 0x68, "IRP.CancelRoutine");
_Static_assert(__builtin_offsetof(IRP, UserBuffer) == 0x70, "IRP.UserBuffer");
_Static_assert(__builtin_offsetof(IRP, Tail.Overlay.ListEntry) == 0xa8, "IRP.ListEntry");
_Static_assert(__builtin_offsetof(IRP, Tail.Overlay.CurrentStackLocation) == 0xb8, "IRP.CurrentStackLocation");
_Static_assert(sizeof(IRP) == 0xd0, "sizeof IRP");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.IoControlCode) == 0x18, "IOSL.IoControlCode");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.Type3InputBuffer) == 0x20, "IOSL.Type3");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, Parameters.Power.Type) == 0x10, "IOSL.Power.Type");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, CompletionRoutine) == 0x38, "IOSL.CompletionRoutine");
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x48, "sizeof IOSL");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, Flags) == 0x30, "DO.Flags");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, DeviceExtension) == 0x40, "DO.DeviceExtension");
_Static_assert(__builtin_offsetof(DRIVER_OBJECT, DriverExtension) == 0x30, "DRV.DriverExtension");
_Static_assert(__builtin_offsetof(DRIVER_OBJECT, MajorFunction) == 0x70, "DRV.MajorFunction");
_Static_assert(__builtin_offsetof(DRIVER_EXTENSION, AddDevice) == 8, "DRVEXT.AddDevice");

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_DEVICE_CONTROL           0x0e
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0f
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_PNP                      0x1b
#define IRP_MN_START_DEVICE             0x00
#define IRP_MN_QUERY_REMOVE_DEVICE      0x01
#define IRP_MN_REMOVE_DEVICE            0x02
#define IRP_MN_STOP_DEVICE              0x04
#define IRP_MN_SURPRISE_REMOVAL         0x17

#define SL_PENDING_RETURNED     0x01
#define SL_INVOKE_ON_CANCEL     0x20
#define SL_INVOKE_ON_SUCCESS    0x40
#define SL_INVOKE_ON_ERROR      0x80
#define DO_DEVICE_INITIALIZING  0x00000080
#define DO_POWER_PAGABLE        0x00002000
#define IO_NO_INCREMENT         0

#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120)
#define STATUS_DELETE_PENDING           ((NTSTATUS)0xC0000056)

static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return irp->Tail.Overlay.CurrentStackLocation; }
static inline PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP irp) { return irp->Tail.Overlay.CurrentStackLocation - 1; }
static inline void IoSkipCurrentIrpStackLocation(PIRP irp) { irp->CurrentLocation++; irp->Tail.Overlay.CurrentStackLocation++; }
static inline void IoMarkIrpPending(PIRP irp) { IoGetCurrentIrpStackLocation(irp)->Control |= SL_PENDING_RETURNED; }
static inline void IoCopyCurrentIrpStackLocationToNext(PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp), n = s - 1;
    { ULONG_PTR *d = (ULONG_PTR *)n; const ULONG_PTR *q = (const ULONG_PTR *)s; int i; for (i = 0; i < 7; i++) d[i] = q[i]; }
    n->Control = 0;
}
static inline void IoSetCompletionRoutine(PIRP irp, PIO_COMPLETION_ROUTINE r, PVOID ctx, int ok, int err, int cancel)
{
    PIO_STACK_LOCATION n = IoGetNextIrpStackLocation(irp);
    n->CompletionRoutine = r;
    n->Context = ctx;
    n->Control = (ok ? SL_INVOKE_ON_SUCCESS : 0) | (err ? SL_INVOKE_ON_ERROR : 0) | (cancel ? SL_INVOKE_ON_CANCEL : 0);
}
static inline PDRIVER_CANCEL IoSetCancelRoutine(PIRP irp, PDRIVER_CANCEL r)
{
    return __atomic_exchange_n(&irp->CancelRoutine, r, __ATOMIC_SEQ_CST);
}
static inline void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static inline int IsListEmpty(const LIST_ENTRY *h) { return h->Flink == h; }
static inline void RemoveEntryList(PLIST_ENTRY e) { PLIST_ENTRY f = e->Flink, b = e->Blink; b->Flink = f; f->Blink = b; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e) { PLIST_ENTRY b = h->Blink; e->Flink = h; e->Blink = b; b->Flink = e; h->Blink = e; }
static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h) { PLIST_ENTRY e = h->Flink; RemoveEntryList(e); return e; }
#define CONTAINING_RECORD(addr, type, field) ((type *)((PCHAR)(addr) - __builtin_offsetof(type, field)))

NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID PoStartNextPowerIrp(PIRP Irp);
VOID IoReleaseCancelSpinLock(KIRQL Irql);
VOID KeInitializeEvent(PKEVENT Event, ULONG Type /* 0 NotificationEvent */, BOOLEAN State);
LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason /* 0 Executive */, CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock);
VOID KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
ULONG ExSetTimerResolution(ULONG DesiredTime, BOOLEAN SetResolution);
typedef VOID (*PKSTART_ROUTINE)(PVOID StartContext);
NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                              PVOID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);
NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK DesiredAccess, PVOID ObjectType, CHAR AccessMode, PVOID *Object, PVOID HandleInfo);
LONG_PTR ObfDereferenceObject(PVOID Object);
NTSTATUS ZwOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
typedef struct _KEY_VALUE_PARTIAL_INFORMATION { ULONG TitleIndex; ULONG Type; ULONG DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION;
NTSTATUS ZwQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass /* 2 = Partial */, PVOID KeyValueInformation,
                         ULONG Length, PULONG ResultLength);
#define KEY_READ 0x20019
#define REG_DWORD 4

// ---- HID minidriver (hidport.h) ----
#define HID_REVISION 0x00000001
typedef struct _HID_MINIDRIVER_REGISTRATION {
    ULONG Revision;
    PDRIVER_OBJECT DriverObject;
    PUNICODE_STRING RegistryPath;
    ULONG DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    UCHAR Reserved[3];
} HID_MINIDRIVER_REGISTRATION, *PHID_MINIDRIVER_REGISTRATION;
_Static_assert(sizeof(HID_MINIDRIVER_REGISTRATION) == 32, "HID_MINIDRIVER_REGISTRATION");
typedef struct _HID_DEVICE_EXTENSION {
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT NextDeviceObject;
    PVOID MiniDeviceExtension;
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;
NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION MinidriverRegistration);

#pragma pack(push, 1)
typedef struct _HID_DESCRIPTOR {
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdHID;
    UCHAR bCountry;
    UCHAR bNumDescriptors;
    struct { UCHAR bReportType; USHORT wReportLength; } DescriptorList[1];
} HID_DESCRIPTOR;
#pragma pack(pop)
_Static_assert(sizeof(HID_DESCRIPTOR) == 9, "HID_DESCRIPTOR");
typedef struct _HID_DEVICE_ATTRIBUTES {
    ULONG Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
    USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES;
_Static_assert(sizeof(HID_DEVICE_ATTRIBUTES) == 32, "HID_DEVICE_ATTRIBUTES");
typedef struct _HID_XFER_PACKET { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; } HID_XFER_PACKET;

// CTL_CODE(FILE_DEVICE_KEYBOARD, n, METHOD_NEITHER, FILE_ANY_ACCESS) / METHOD_OUT_DIRECT for the get/set report family
#define IOCTL_HID_GET_DEVICE_DESCRIPTOR     0x000B0003
#define IOCTL_HID_GET_REPORT_DESCRIPTOR     0x000B0007
#define IOCTL_HID_READ_REPORT               0x000B000B
#define IOCTL_HID_WRITE_REPORT              0x000B000F
#define IOCTL_HID_GET_STRING                0x000B0013
#define IOCTL_HID_ACTIVATE_DEVICE           0x000B001F
#define IOCTL_HID_DEACTIVATE_DEVICE         0x000B0023
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES     0x000B0027
#define IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST 0x000B002B
#define IOCTL_HID_GET_FEATURE               0x000B0192
#define HID_STRING_ID_IMANUFACTURER 14
#define HID_STRING_ID_IPRODUCT      15
#define HID_STRING_ID_ISERIALNUMBER 16
