// J606F: read-only probe of the Qualcomm SCM (TrustZone) interface from Windows, first step towards remote processors (ADSP: sound, sensors;
// MPSS: Wi-Fi). Only information calls, as Linux qcom_scm.c does at probe time:
//   - SMC64 convention check: INFO/IS_CALL_AVAIL (svc 6, cmd 1) on itself;
//   - IS_CALL_AVAIL for PIL init_image/mem_setup/auth_and_reset/shutdown/is_supported, MP assign/restore_sec_cfg, BOOT set_dload_mode (query only);
//   - PIL PAS_IS_SUPPORTED for pas ids 1 (ADSP), 4 (MPSS), 18 (CDSP); run_50: scan of ids 0..40, fast/smc32 variants, retry after 3 s.
// Nothing is loaded, assigned or changed. SMC: x0 = 0x42000000 | (svc << 8) | cmd (SMCCC std call, SMC64, owner SIP), x1 = arginfo, x2.. args.
// Log: C:\j606f\out\scmprobe.txt.
#define RPM_NO_ENTRY
#define RPM_LOG_PATH L"\\??\\C:\\j606f\\out\\scmprobe.txt"
#include "../rpm/rpm.c"

typedef unsigned long long U64;

static U64 Smc(U64 a0, U64 a1, U64 a2, U64 a3, U64 *r1, U64 *r2, U64 *r3)
{
    register U64 x0 __asm__("x0") = a0;
    register U64 x1 __asm__("x1") = a1;
    register U64 x2 __asm__("x2") = a2;
    register U64 x3 __asm__("x3") = a3;
    register U64 x4 __asm__("x4") = 0;
    register U64 x5 __asm__("x5") = 0;
    register U64 x6 __asm__("x6") = 0;
    register U64 x7 __asm__("x7") = 0;

    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    *r1 = x1;
    *r2 = x2;
    *r3 = x3;
    return x0;
}

static int _snprintf_ok(char *b, ULONG n, const char *fmt, int id, long long a, unsigned long long c)
{
    static const char hex[] = "0123456789";
    int k = 0, j;
    char t[24];
    (void)fmt;
    if (n < 40)
        return 0;
    b[k++] = ' ';
    j = 0; do { t[j++] = hex[id % 10]; id /= 10; } while (id);
    while (j) b[k++] = t[--j];
    b[k++] = ':';
    if (a < 0) { b[k++] = '-'; a = -a; }
    j = 0; do { t[j++] = hex[a % 10]; a /= 10; } while (a);
    while (j) b[k++] = t[--j];
    b[k++] = '/';
    j = 0; do { t[j++] = hex[c % 10]; c /= 10; } while (c);
    while (j) b[k++] = t[--j];
    return k;
}

#define FNID(s, c) ((((s) & 0xff) << 8) | ((c) & 0xff))
#define SMC64_SIP(s, c) (0x42000000ull | FNID(s, c))

static int Avail(int svc, int cmd, const char *name)
{
    U64 r1, r2, r3, r0 = Smc(SMC64_SIP(6, 1), 1, FNID(svc, cmd) | (2u << 24), 0, &r1, &r2, &r3);

    Log("is_call_available %-28s (svc %02x cmd %02x): x0 %lld x1 %llu", name, svc, cmd, (long long)r0, r1);
    return r0 == 0 && r1;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    static const int ids[] = { 1, 4, 18 };
    static const char *names[] = { "ADSP", "MPSS (modem)", "CDSP" };
    int i;

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    Log("=== j606f scmprobe (read-only SCM queries)");
    if (!Avail(6, 1, "info.is_call_available")) {
        Log("SMC64 convention not answered -> stop");
        return STATUS_UNSUCCESSFUL;
    }
    Avail(2, 0x01, "pil.init_image");
    Avail(2, 0x02, "pil.mem_setup");
    Avail(2, 0x05, "pil.auth_and_reset");
    Avail(2, 0x06, "pil.shutdown");
    Avail(2, 0x07, "pil.is_supported");
    Avail(2, 0x0a, "pil.mss_reset");
    Avail(0x0c, 0x02, "mp.restore_sec_cfg");
    Avail(0x0c, 0x16, "mp.assign");
    Avail(0x0c, 0x05, "mp.iommu_secure_ptbl_size");
    Avail(0x01, 0x10, "boot.set_dload_mode");
    for (i = 0; i < 3; i++) {
        U64 r1, r2, r3, r0 = Smc(SMC64_SIP(2, 7), 1, ids[i], 0, &r1, &r2, &r3);
        Log("pas_is_supported %2d %-14s: x0 %lld x1 %llu x2 %llx x3 %llx", ids[i], names[i], (long long)r0, r1, r2, r3);
    }
    // run_50: all ids (std SMC64), then other call variants for 1/4
    {
        char line[200];
        int n = 0, id;
        for (id = 0; id <= 40; id++) {
            U64 r1, r2, r3, r0 = Smc(SMC64_SIP(2, 7), 1, id, 0, &r1, &r2, &r3);
            if (r0 != 0 || r1 != 0)
                n += _snprintf_ok(line + n, sizeof(line) - n, " %d:%lld/%llu", id, (long long)r0, r1);
        }
        line[n] = 0;
        Log("pas_is_supported scan 0..40 (std smc64), non-zero answers:%s", n ? line : " none");
    }
    {
        static const struct { U64 fn; const char *what; } v[] = {
            { 0xC2000207ull, "fast smc64" }, { 0x02000207ull, "std smc32" }, { 0x82000207ull, "fast smc32" },
        };
        int k;
        for (k = 0; k < 3; k++)
            for (i = 0; i < 2; i++) {
                U64 r1, r2, r3, r0 = Smc(v[k].fn, 1, ids[i], 0, &r1, &r2, &r3);
                Log("pas_is_supported %2d via %-10s (%08llx): x0 %lld x1 %llu", ids[i], v[k].what, v[k].fn, (long long)r0, r1);
            }
    }
    SleepMs(3000);
    for (i = 0; i < 3; i++) {
        U64 r1, r2, r3, r0 = Smc(SMC64_SIP(2, 7), 1, ids[i], 0, &r1, &r2, &r3);
        Log("after 3 s: pas_is_supported %2d: x0 %lld x1 %llu", ids[i], (long long)r0, r1);
    }
    Log("=== done");
    return STATUS_UNSUCCESSFUL;                 // nothing to keep loaded
}
