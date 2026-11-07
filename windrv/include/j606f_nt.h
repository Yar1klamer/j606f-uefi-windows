// Minimal Windows kernel definitions for J606F drivers built with llvm-mingw (aarch64): the mingw DDK headers stop at
// "#error Unknown Architecture" on ARM64. Layouts follow the public WDK documentation (64-bit, natural alignment).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NTAPI
#define VOID void
typedef void *PVOID, *HANDLE, **PHANDLE;
typedef char CHAR, *PCHAR;
typedef const char *PCSTR;
typedef uint8_t UCHAR, BOOLEAN, *PUCHAR;
typedef int16_t CSHORT;
typedef uint16_t USHORT, WCHAR, *PWCH, *PWSTR;
typedef int32_t LONG, NTSTATUS;
typedef uint32_t ULONG, *PULONG, ACCESS_MASK;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG, ULONG_PTR, SIZE_T, *PSIZE_T;
typedef union { struct { ULONG LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef LARGE_INTEGER PHYSICAL_ADDRESS;

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5)
#define UNREFERENCED_PARAMETER(p) ((void)(p))

typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWCH Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
#define OBJ_CASE_INSENSITIVE 0x00000040
#define OBJ_KERNEL_HANDLE 0x00000200
#define InitializeObjectAttributes(p, n, a, r, s) do { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); (p)->RootDirectory = (r); (p)->Attributes = (a); \
    (p)->ObjectName = (n); (p)->SecurityDescriptor = (s); (p)->SecurityQualityOfService = NULL; } while (0)

typedef struct _IO_STATUS_BLOCK { union { NTSTATUS Status; PVOID Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

#define SYNCHRONIZE 0x00100000
#define FILE_APPEND_DATA 0x0004
#define FILE_WRITE_DATA 0x0002
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_SHARE_READ 0x00000001
#define FILE_OPEN_IF 0x00000003
#define FILE_OVERWRITE_IF 0x00000005
#define FILE_WRITE_THROUGH 0x00000002
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE 0x00000040

struct _DEVICE_OBJECT;
struct _IRP;
typedef struct _DRIVER_OBJECT *PDRIVER_OBJECT;
typedef VOID (*PDRIVER_UNLOAD)(PDRIVER_OBJECT DriverObject);
typedef NTSTATUS (*PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp);
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b
typedef struct _DRIVER_OBJECT {
    CSHORT Type;
    CSHORT Size;
    struct _DEVICE_OBJECT *DeviceObject;
    ULONG Flags;
    PVOID DriverStart;
    ULONG DriverSize;
    PVOID DriverSection;
    PVOID DriverExtension;
    UNICODE_STRING DriverName;
    PUNICODE_STRING HardwareDatabase;
    PVOID FastIoDispatch;
    PVOID DriverInit;
    PVOID DriverStartIo;
    PDRIVER_UNLOAD DriverUnload;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
} DRIVER_OBJECT;

typedef enum _MEMORY_CACHING_TYPE { MmNonCached = 0, MmCached = 1, MmWriteCombined = 2 } MEMORY_CACHING_TYPE;
#define PAGE_READWRITE 0x04
#define PAGE_NOCACHE 0x200

ULONG DbgPrint(PCSTR Format, ...);
VOID RtlInitUnicodeString(PUNICODE_STRING Destination, const WCHAR *Source);
NTSTATUS ZwCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                      PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
                      PVOID EaBuffer, ULONG EaLength);
NTSTATUS ZwWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                     ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);
NTSTATUS ZwClose(HANDLE Handle);
NTSTATUS ZwReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                    ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);
PVOID ExAllocatePool2(ULONGLONG Flags, SIZE_T NumberOfBytes, ULONG Tag);
VOID ExFreePoolWithTag(PVOID P, ULONG Tag);
#define POOL_FLAG_NON_PAGED 0x0000000000000040ULL
VOID KeQuerySystemTimePrecise(PLARGE_INTEGER CurrentTime);
NTSTATUS KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
VOID KeStallExecutionProcessor(ULONG MicroSeconds);
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, ULONG Protect);
VOID MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes);
int _vsnprintf(char *buffer, size_t count, const char *format, __builtin_va_list argptr);
