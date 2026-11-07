// J606F: user-mode helper for the loader experiment. Loads the named services with NtLoadDriver and prints the raw NTSTATUS
// (sc.exe only shows the Win32 code), unloads each one that loaded. Run as LocalSystem (agent): ntload.exe svc1 svc2 ...
#include <windows.h>
#include <stdio.h>

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
__declspec(dllimport) LONG NTAPI NtLoadDriver(USTR *);
__declspec(dllimport) LONG NTAPI NtUnloadDriver(USTR *);
__declspec(dllimport) LONG NTAPI RtlAdjustPrivilege(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);

int wmain(int argc, wchar_t **argv)
{
    BOOLEAN was;
    wchar_t path[256];
    USTR us;
    LONG st;
    int i;

    printf("adjust SeLoadDriverPrivilege: %08lx\n", (unsigned long)RtlAdjustPrivilege(10, TRUE, FALSE, &was));
    for (i = 1; i < argc; i++) {
        _snwprintf(path, 255, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%ls", argv[i]);
        path[255] = 0;
        us.Buffer = path;
        us.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        us.MaximumLength = us.Length + sizeof(wchar_t);
        st = NtLoadDriver(&us);
        printf("%ls: NtLoadDriver %08lx", argv[i], (unsigned long)st);
        if (st >= 0)
            printf(", NtUnloadDriver %08lx", (unsigned long)NtUnloadDriver(&us));
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
