// J606F: loader experiment. Same as hello.c, plus a pointer table so the image has base relocations; VARIANT names the build.
#include "j606f_nt.h"

#ifndef VARIANT
#define VARIANT "?"
#endif

static volatile int g_idx;

// writable table + volatile index: the compiler cannot fold it, so the linker must emit DIR64 base relocations
const char *g_lines[] = { "j606f hellov " VARIANT ": DriverEntry OK\r\n", "j606f hellov " VARIANT ": unload\r\n" };

static void WriteLine(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\hello_drv.txt");
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attrs, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(file, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
        ZwClose(file);
    }
}

static VOID HelloUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    WriteLine(g_lines[g_idx + 1]);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = HelloUnload;
    WriteLine(g_lines[g_idx]);
    return STATUS_SUCCESS;
}
