#!/bin/sh
# Apply the J606F changes to a fresh copy of the MS KMDOD sample (run in the sample directory on the build host). Idempotent.
set -eu
grep -q J606fQueryInterface bdd_ddi.cxx || perl -0pi -e 's/(extern "C"\nNTSTATUS\nDriverEntry\()/extern "C" void J606fInit(void);\nextern "C" void J606fLog(const char *fmt, ...);\nextern "C" NTSTATUS APIENTRY J606fQueryInterface(CONST PVOID, PQUERY_INTERFACE);\n\n$1/; s/(    InitialData\.DxgkDdiSystemDisplayWrite           = BddDdiSystemDisplayWrite;\n)/$1    InitialData.DxgkDdiQueryInterface               = J606fQueryInterface;\n    J606fInit();\n    J606fLog("=== j606fdsp DriverEntry (KMDOD + DCS brightness), interface version %08lx", (ULONG)DXGKDDI_INTERFACE_VERSION);\n/' bdd_ddi.cxx
grep -q "J606F: internal panel" bdd.cxx || perl -0pi -e 's/= m_CurrentModes\[0\]\.Flags\.IsInternal \? D3DKMDT_VOT_INTERNAL : D3DKMDT_VOT_OTHER;/= D3DKMDT_VOT_INTERNAL;  \/\/ J606F: internal panel (brightness slider)/; s/(    m_Flags\.DriverStarted = TRUE;\n)/    J606fLog("StartDevice: POST display %lux%lu pitch %lu format %lu", m_CurrentModes[0].DispInfo.Width, m_CurrentModes[0].DispInfo.Height, m_CurrentModes[0].DispInfo.Pitch, (ULONG)m_CurrentModes[0].DispInfo.ColorFormat);\n$1/; s/(#include "BDD\.hxx")/$1\nextern "C" void J606fLog(const char *fmt, ...);/' bdd.cxx
grep -c J606f bdd.cxx bdd_ddi.cxx
# v2: log StartDevice steps; if dxgkrnl gives no POST display (root device that is not BasicDisplay), use the UEFI GOP frame buffer
# (Mu-Silicium SimpleFbDxe: "Display Reserved" 0x5c000000, 1200x2000, 32 bpp BGRX, pitch 4800; pmOS simpledrm sees the same).
grep -q "J606F fallback" bdd.cxx || perl -0pi -e 's/(    Status = m_DxgkInterface\.DxgkCbAcquirePostDisplayOwnership\(m_DxgkInterface\.DeviceHandle, &\(m_CurrentModes\[0\]\.DispInfo\)\);\n)/    J606fLog("StartDevice: enter");\n$1    J606fLog("StartDevice: AcquirePostDisplayOwnership %08lx, %lux%lu pa %llx", (ULONG)Status, m_CurrentModes[0].DispInfo.Width, m_CurrentModes[0].DispInfo.Height, (ULONGLONG)m_CurrentModes[0].DispInfo.PhysicAddress.QuadPart);\n    if (!NT_SUCCESS(Status) || m_CurrentModes[0].DispInfo.Width == 0)\n    {\n        \/\/ J606F fallback: the UEFI GOP frame buffer\n        m_CurrentModes[0].DispInfo.Width = 1200;\n        m_CurrentModes[0].DispInfo.Height = 2000;\n        m_CurrentModes[0].DispInfo.Pitch = 1200 * 4;\n        m_CurrentModes[0].DispInfo.ColorFormat = D3DDDIFMT_A8R8G8B8;\n        m_CurrentModes[0].DispInfo.PhysicAddress.QuadPart = 0x5c000000;\n        m_CurrentModes[0].DispInfo.TargetId = 0;\n        m_CurrentModes[0].DispInfo.AcpiId = 0;\n        Status = STATUS_SUCCESS;\n        J606fLog("StartDevice: using the UEFI frame buffer 0x5c000000 1200x2000");\n    }\n/' bdd.cxx
grep -q "J606F AddDevice" bdd_ddi.cxx || perl -0pi -e 's/(BddDdiAddDevice\([^)]*\)\n\{\n    PAGED_CODE\(\);\n)/$1    J606fLog("AddDevice (J606F AddDevice)");\n/' bdd_ddi.cxx
grep -c "J606F" bdd.cxx bdd_ddi.cxx
# v3: log every DDI that dxgkrnl calls and its NTSTATUS (Present only the first 5 calls and failures; pointer DDIs and the ISR not logged)
grep -q "J606F ddi log" bdd_ddi.cxx || perl -0pi -e '
  s{\n    return pBDD->(PresentDisplayOnly)\((.*?)\);}{\n    { /* J606F ddi log */ static LONG c; NTSTATUS s_ = pBDD->$1($2); if (!NT_SUCCESS(s_) || InterlockedIncrement(&c) <= 5) J606fLog("ddi $1 %08lx", (ULONG)s_); return s_; }}g;
  s{\n    return pBDD->(QueryAdapterInfo)\((.*?)\);}{\n    { /* J606F ddi log */ NTSTATUS s_ = pBDD->$1($2); J606fLog("ddi $1 type %d size %u -> %08lx", (int)pQueryAdapterInfo->Type, pQueryAdapterInfo->OutputDataSize, (ULONG)s_); return s_; }}g;
  s{\n    return pBDD->((?!InterruptRoutine|SetPointer|PresentDisplayOnly|QueryAdapterInfo|SystemDisplay)\w+)\((.*?)\);}{\n    { /* J606F ddi log */ NTSTATUS s_ = pBDD->$1($2); J606fLog("ddi $1 %08lx", (ULONG)s_); return s_; }}g;
' bdd_ddi.cxx
grep -c "J606F ddi log" bdd_ddi.cxx
# v4: an internal panel must be HpdAwarenessAlwaysConnected (run_26: StartDevice OK, QueryChildRelations OK, then dxgkrnl StopDevice → code 43)
grep -q "J606F always connected" bdd.cxx || perl -pi -e 's/(ChildCapabilities\.HpdAwareness = )HpdAwarenessInterruptible;/${1}HpdAwarenessAlwaysConnected;  \/\/ J606F always connected (internal panel)/' bdd.cxx
grep -c "J606F always connected" bdd.cxx
# v5: output type and HPD selectable for experiments: J606F_VOT=INTERNAL|OTHER (default OTHER, as the sample on a non-internal POST display),
# J606F_HPD=AlwaysConnected|Interruptible (default Interruptible). run_27: INTERNAL + AlwaysConnected → still StopDevice after QueryChildRelations.
VOT=${J606F_VOT:-OTHER}
HPD=${J606F_HPD:-Interruptible}
perl -pi -e "s/(VideoOutput\.InterfaceTechnology = )D3DKMDT_VOT_\w+;.*\$/\${1}D3DKMDT_VOT_$VOT;  \/\/ J606F: internal panel (brightness slider) — J606F_VOT/; s/(ChildCapabilities\.HpdAwareness = )HpdAwareness\w+;.*\$/\${1}HpdAwareness$HPD;  \/\/ J606F always connected (internal panel) — J606F_HPD/" bdd.cxx
grep -n "J606F_VOT\|J606F_HPD" bdd.cxx | cut -c1-140
# v6: always synchronous present. The sample alternates sync/async; async completion is reported through DxgkCbSynchronizeExecution
# (a fake interrupt), but a root device has no interrupt → the present never completes → dxgkrnl stops the adapter (run_28: Present 103, StopDevice).
grep -q "J606F synchronous present" blthw.cxx || perl -pi -e 's/^(\s*)m_SynchExecution = !m_SynchExecution;/${1}m_SynchExecution = TRUE;  \/\/ J606F synchronous present (no interrupt on a root device)/' blthw.cxx
grep -c "J606F synchronous present" blthw.cxx
# v7: log the resources PnP gave the adapter (run_31 forces Memory 0x5C000000 via Enum\...\LogConf\ForcedConfig)
grep -q "J606F resources" bdd.cxx || perl -0pi -e 's/(    J606fLog\("StartDevice: enter"\);\n)/$1    { \/\/ J606F resources\n        PCM_RESOURCE_LIST r = m_DeviceInfo.TranslatedResourceList;\n        if (r && r->Count) {\n            PCM_PARTIAL_RESOURCE_LIST p = &r->List[0].PartialResourceList;\n            J606fLog("StartDevice: %lu resources", p->Count);\n            for (ULONG i = 0; i < p->Count && i < 4; i++)\n                J606fLog("  res %lu type %u start %llx len %lx", i, (UINT)p->PartialDescriptors[i].Type, (ULONGLONG)p->PartialDescriptors[i].u.Memory.Start.QuadPart, p->PartialDescriptors[i].u.Memory.Length);\n        } else J606fLog("StartDevice: no resources");\n    }\n/' bdd.cxx
grep -c "J606F resources" bdd.cxx
# v8: child AcpiUid selectable (J606F_ACPIUID, default 0). With UEFI v8 the adapter is ACPI\JDSP0001 (\_SB.DSP0, _DOD 0x80010400, LCD0 _ADR 0x400).
UID=${J606F_ACPIUID:-0}
perl -pi -e "s/^(\s*pChildRelations\[ChildIndex\]\.AcpiUid = )\S+;.*\$/\${1}$UID;  \/\/ J606F_ACPIUID/" bdd.cxx
grep -n "J606F_ACPIUID" bdd.cxx | cut -c1-110
