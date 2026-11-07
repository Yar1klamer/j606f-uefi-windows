// J606F: smoke test for kernel drivers built on Linux (llvm-mingw, aarch64). On load it writes C:\j606f\out\hello_drv.txt and stays loaded.
#include "j606f_nt.h"

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
                                FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(file, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
        ZwClose(file);
    }
}

static VOID HelloUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    WriteLine("j606f hello: unload\r\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = HelloUnload;
    DbgPrint("j606f hello: DriverEntry\n");
    WriteLine("j606f hello: DriverEntry OK (built with llvm-mingw aarch64)\r\n");
    return STATUS_SUCCESS;
}
