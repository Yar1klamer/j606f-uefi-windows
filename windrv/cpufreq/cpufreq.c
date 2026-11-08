// J606F CPU frequency (Windows ARM64 kernel driver, test-signed, legacy service).
// The firmware cpufreq-hw (OSM-lite) is enabled (run_36): domain0 cpu0-3 0xf521000, domain1 cpu4-7 0xf523000; LUT = Linux
// (dom0 300..1804.8 MHz, 8 rows; dom1 300..2016 MHz, 9 rows). Windows has no driver for it: after boot dom0 sits at index 4 (1305.6 MHz)
// and dom1 at index 0 (300 MHz). Linux qcom-cpufreq-hw only writes the LUT index to REG_PERF_STATE (0x920); voltages are the firmware's.
// This driver does the same:
//   - service name ending "cpub": benchmark/test — for each domain, every LUT index: write perf_state, read back, time a fixed loop on
//     cpu0 / cpu4 (KeSetSystemAffinityThreadEx), log; then the governor runs for the test time.
//   - governor thread, 200 ms: per domain the busiest CPU's load (SystemProcessorPerformanceInformation): >= 80 % -> top index, else the
//     lowest index with freq >= cur * load / 70 (ondemand-like). Thermal cap from tsens v2 (max of 16 sensors): >= 85 C cap -1 index per
//     period, >= 95 C cap = lowest, <= 75 C cap +1. Service name ending "cpum": always top index (still thermally capped).
//   - unload: restores the perf_state values found at load.
// Writes: only REG_PERF_STATE of the two domains. Log: C:\j606f\out\cpufreq.txt.
#include "j606f_wdm.h"

typedef ULONG_PTR KAFFINITY;
PVOID MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName);
void KeQuerySystemTimePrecise(PLARGE_INTEGER CurrentTime);
NTSTATUS ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);

// SystemProcessorPerformanceInformation (8): per logical CPU
typedef struct {
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
} SPPI;

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\cpufreq.txt");
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attrs, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(file, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
        ZwClose(file);
    }
}

static void Log(const char *fmt, ...)
{
    char line[256];
    __builtin_va_list ap;
    ULONG n = 0;

    __builtin_va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    __builtin_va_end(ap);
    line[sizeof(line) - 3] = 0;
    while (line[n])
        n++;
    line[n] = '\r';
    line[n + 1] = '\n';
    line[n + 2] = 0;
    LogRaw(line);
}

static ULONG Rd(volatile UCHAR *base, ULONG off)
{
    ULONG v = *(volatile ULONG *)(base + off);
    __asm__ volatile("dmb sy" ::: "memory");
    return v;
}

static void Wr(volatile UCHAR *base, ULONG off, ULONG v)
{
    __asm__ volatile("dmb sy" ::: "memory");
    *(volatile ULONG *)(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void SleepMs(LONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(0, FALSE, &t);
}

static volatile UCHAR *Map(ULONG pa, SIZE_T size)
{
    PHYSICAL_ADDRESS p;
    volatile UCHAR *va;

    p.QuadPart = pa;
    va = (volatile UCHAR *)MmMapIoSpaceEx(p, size, PAGE_READWRITE | PAGE_NOCACHE);
    if (!va)
        Log("map %08lx failed", (unsigned long)pa);
    return va;
}

// ---------------------------------------------------------------------------------------------------------------------------------
#define REG_ENABLE      0x000
#define REG_FREQ_LUT    0x110
#define REG_VOLT_LUT    0x114
#define REG_VOTE        0x704
#define REG_PERF_STATE  0x920
#define LUT_ROW         32

typedef struct {
    volatile UCHAR *b;
    ULONG n;            // number of distinct LUT rows
    ULONG mhz[40];
    ULONG orig, cur;
    int cpu;            // first cpu of the domain (for the benchmark)
} DOMAIN;

static DOMAIN g_dom[2];
static volatile UCHAR *g_tm;

static int DomainInit(DOMAIN *d, ULONG pa, int cpu)
{
    ULONG i, prev = 0xffffffff;

    d->b = Map(pa, 0x1000);
    d->cpu = cpu;
    if (!d->b)
        return -1;
    if ((Rd(d->b, REG_ENABLE) & 1) == 0) {
        Log("domain %08lx: not enabled (%08lx)", (unsigned long)pa, (unsigned long)Rd(d->b, REG_ENABLE));
        return -1;
    }
    for (i = 0; i < 40; i++) {
        ULONG f = Rd(d->b, REG_FREQ_LUT + LUT_ROW * i);
        if (i && f == prev)
            break;
        d->mhz[i] = ((f >> 30) & 3) ? (192 * (f & 0xff)) / 10 : 300;
        prev = f;
    }
    d->n = i;
    d->orig = d->cur = Rd(d->b, REG_PERF_STATE);
    Log("domain %08lx: %lu levels, %lu..%lu MHz, perf_state at load %lu (%lu MHz)", (unsigned long)pa, (unsigned long)d->n,
        (unsigned long)d->mhz[0], (unsigned long)d->mhz[d->n - 1], (unsigned long)d->orig,
        (unsigned long)(d->orig < d->n ? d->mhz[d->orig] : 0));
    return d->n ? 0 : -1;
}

static void DomainSet(DOMAIN *d, ULONG idx)
{
    if (!d->b || idx >= d->n)
        return;
    Wr(d->b, REG_PERF_STATE, idx);
    d->cur = idx;
}

static LONG MaxTemp(void)          // 0.1 C, -1000 if none valid
{
    LONG best = -1000;
    int i;

    if (!g_tm)
        return best;
    for (i = 0; i < 16; i++) {
        ULONG s = Rd(g_tm, 0xa0 + 4 * i);
        LONG t = (LONG)(s & 0xfff);
        if (!((s >> 21) & 1))
            continue;
        if (t & 0x800)
            t -= 0x1000;
        if (t > best)
            best = t;
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// benchmark: a dependent integer loop, timed with the system time (100 ns)
typedef KAFFINITY (*PFN_SETAFF)(KAFFINITY);
typedef VOID (*PFN_REVAFF)(KAFFINITY);
static PFN_SETAFF g_setaff;
static PFN_REVAFF g_revaff;

static ULONG LoopUs(void)
{
    LARGE_INTEGER a, b;
    volatile ULONG x = 1;
    ULONG i, y = 1;

    KeQuerySystemTimePrecise(&a);
    for (i = 0; i < 20000000; i++)
        y = y * 1664525u + 1013904223u;
    x = y;
    KeQuerySystemTimePrecise(&b);
    (void)x;
    return (ULONG)((b.QuadPart - a.QuadPart) / 10);
}

static void Bench(void)
{
    int k;
    ULONG i;

    for (k = 0; k < 2; k++) {
        DOMAIN *d = &g_dom[k];
        KAFFINITY old = 0;
        if (!d->b)
            continue;
        if (g_setaff)
            old = g_setaff((KAFFINITY)1 << d->cpu);
        Log("bench dom%d on cpu%d: at load perf %lu -> loop %lu us", k, d->cpu, (unsigned long)d->orig, (unsigned long)LoopUs());
        for (i = 0; i < d->n; i++) {
            ULONG us, vote;
            DomainSet(d, i);
            SleepMs(20);
            us = LoopUs();
            vote = Rd(d->b, REG_VOTE);
            Log("bench dom%d idx %lu (%lu MHz): perf readback %lu vote %08lx loop %lu us (%lu MHz equiv vs 300)", k, (unsigned long)i,
                (unsigned long)d->mhz[i], (unsigned long)Rd(d->b, REG_PERF_STATE), (unsigned long)vote, (unsigned long)us, 0UL);
        }
        if (g_revaff)
            g_revaff(old);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
static volatile LONG g_stop;
static PVOID g_thread;

// load: busy share of each CPU over the last period from SystemProcessorPerformanceInformation; a domain follows its busiest CPU.
#define NCPU 8
static SPPI g_prev[NCPU];
static int g_have_prev;

static void CpuBusy(ULONG busy[NCPU])          // 0..100
{
    SPPI now[NCPU];
    ULONG got = 0, i;

    for (i = 0; i < NCPU; i++)
        busy[i] = 100;
    if (!NT_SUCCESS(ZwQuerySystemInformation(8, now, sizeof(now), &got)))
        return;
    for (i = 0; i < NCPU && (i + 1) * sizeof(SPPI) <= got; i++) {
        LONGLONG idle = now[i].IdleTime.QuadPart - g_prev[i].IdleTime.QuadPart;
        LONGLONG tot = (now[i].KernelTime.QuadPart - g_prev[i].KernelTime.QuadPart) + (now[i].UserTime.QuadPart - g_prev[i].UserTime.QuadPart);
        if (g_have_prev && tot > 0) {
            LONGLONG b = 100 - (idle * 100) / tot;
            busy[i] = (ULONG)(b < 0 ? 0 : b > 100 ? 100 : b);
        }
        g_prev[i] = now[i];
    }
    g_have_prev = 1;
}

// ondemand-like: busy >= 80 % -> top index; otherwise the lowest index with freq >= cur_freq * busy / 70 (load near 70 % at the new speed).
// Thermal guard on top: >= 85 C caps one index lower per period (cap returns +1 per period at <= 75 C), >= 95 C -> index 0.
static ULONG g_cap[2];

static VOID GovThread(PVOID ctx)
{
    ULONG ticks = 0, busy[NCPU], fixed = (ULONG)(ULONG_PTR)ctx;
    int k;

    for (k = 0; k < 2; k++)
        g_cap[k] = g_dom[k].n ? g_dom[k].n - 1 : 0;
    CpuBusy(busy);
    Log("governor: start (%s), temp %ld", fixed ? "fixed max" : "load + thermal", (long)MaxTemp());
    while (!g_stop) {
        LONG t = MaxTemp();
        CpuBusy(busy);
        for (k = 0; k < 2; k++) {
            DOMAIN *d = &g_dom[k];
            ULONG want, b = 0, i, target;
            if (!d->b)
                continue;
            for (i = 0; i < 4; i++)
                if (busy[d->cpu + i] > b)
                    b = busy[d->cpu + i];
            if (t >= 950)
                g_cap[k] = 0;
            else if (t >= 850 && g_cap[k] > 0)
                g_cap[k]--;
            else if (t <= 750 && g_cap[k] + 1 < d->n)
                g_cap[k]++;
            if (fixed || b >= 80) {
                want = d->n - 1;
            } else {
                target = (d->mhz[d->cur] * b) / 70;
                for (want = 0; want + 1 < d->n && d->mhz[want] < target; want++)
                    ;
            }
            if (want > g_cap[k])
                want = g_cap[k];
            if (want != d->cur)
                DomainSet(d, want);
        }
        ++ticks;
        if ((ticks <= 1500 && ticks % 50 == 0) || ticks % 3000 == 0)       // every 10 s for 5 min, then every 10 min
            Log("governor: temp %ld.%ld C busy %lu %lu %lu %lu | %lu %lu %lu %lu -> dom0 %lu MHz dom1 %lu MHz cap %lu/%lu", (long)(t / 10), (long)(t % 10),
                busy[0], busy[1], busy[2], busy[3], busy[4], busy[5], busy[6], busy[7],
                (unsigned long)g_dom[0].mhz[g_dom[0].cur], (unsigned long)g_dom[1].mhz[g_dom[1].cur], (unsigned long)g_cap[0], (unsigned long)g_cap[1]);
        SleepMs(200);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID Unload(PDRIVER_OBJECT DriverObject)
{
    int k;

    UNREFERENCED_PARAMETER(DriverObject);
    if (g_thread) {
        g_stop = 1;
        KeWaitForSingleObject(g_thread, 0, 0, FALSE, NULL);
        ObfDereferenceObject(g_thread);
        g_thread = NULL;
    }
    for (k = 0; k < 2; k++)
        if (g_dom[k].b) {
            DomainSet(&g_dom[k], g_dom[k].orig);
            Log("unload: dom%d perf_state restored to %lu (readback %lu)", k, (unsigned long)g_dom[k].orig,
                (unsigned long)Rd(g_dom[k].b, REG_PERF_STATE));
        }
    Log("=== unload, temp %ld", (long)MaxTemp());
}

static int EndsWith(PUNICODE_STRING p, const char *s)
{
    USHORT n = p->Length / sizeof(WCHAR), m = 0, i;

    while (s[m])
        m++;
    if (n < m)
        return 0;
    for (i = 0; i < m; i++)
        if ((p->Buffer[n - m + i] | 0x20) != s[i])
            return 0;
    return 1;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING name;
    int bench = RegistryPath && RegistryPath->Buffer && EndsWith(RegistryPath, "cpub");
    NTSTATUS st;
    HANDLE h;

    DriverObject->DriverUnload = Unload;
    Log("=== j606f cpufreq start, mode %s", bench ? "benchmark + governor" : "governor");
    RtlInitUnicodeString(&name, L"KeSetSystemAffinityThreadEx");
    g_setaff = (PFN_SETAFF)MmGetSystemRoutineAddress(&name);
    RtlInitUnicodeString(&name, L"KeRevertToUserAffinityThreadEx");
    g_revaff = (PFN_REVAFF)MmGetSystemRoutineAddress(&name);
    g_tm = Map(0x04411000, 0x1000);
    Log("temp max %ld (0.1 C)", (long)MaxTemp());
    DomainInit(&g_dom[0], 0x0f521000, 0);
    DomainInit(&g_dom[1], 0x0f523000, 4);
    if (!g_dom[0].b && !g_dom[1].b)
        return STATUS_SUCCESS;
    if (bench)
        Bench();
    st = PsCreateSystemThread(&h, 0x1FFFFF, NULL, NULL, NULL, GovThread, (PVOID)(ULONG_PTR)EndsWith(RegistryPath, "cpum"));
    if (NT_SUCCESS(st)) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(h, 0x1FFFFF, NULL, 0, &g_thread, NULL)))
            g_thread = NULL;
        ZwClose(h);
    }
    Log("governor thread %08lx", (unsigned long)st);
    return STATUS_SUCCESS;
}
