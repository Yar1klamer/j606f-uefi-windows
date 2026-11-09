// J606F Wi-Fi: NDIS 6.30 Ethernet miniport. The radio side lives in btbus (wlan.inc); this driver only moves Ethernet frames across a kernel callback object.
//  Bound over \Callback\J606fWlan: op 1 from here hands btbus a J606F_WIFI_BIND (our MAC + rx/link functions), btbus fills in its tx function.
//  The adapter MAC is a locally administered address generated once and kept in Services\j606fnet\Parameters; it is never logged.
#define NDIS_MINIPORT_DRIVER 1
#define NDIS630_MINIPORT 1
#define NDIS_WDM 1
#include <ndis.h>

#define J_MTU        1500
#define J_FRAME      1514
#define J_LINK       (72200000ULL)
#define J_TAG        'N60J'

typedef struct {
    UCHAR mac[6];
    void (*rx)(void *ctx, const UCHAR *f, ULONG n);
    void (*link)(void *ctx, int up);
    void *ctx;
    int (*tx)(const UCHAR *f, ULONG n);
} J606F_WIFI_BIND;

typedef struct {
    NDIS_HANDLE h, pool;
    PNET_BUFFER_LIST nbl;
    PMDL mdl;
    UCHAR *rxbuf;
    UCHAR mac[6];
    volatile LONG paused, up;
    ULONG filter, lookahead;
    J606F_WIFI_BIND bind;
    ULONG64 tx_ok, rx_ok, tx_bad, rx_bad, rx_nobuf;
    UCHAR txbuf[J_FRAME + 64];
    KSPIN_LOCK txlock;
} ADAPTER;

static NDIS_HANDLE g_drv;
static PCALLBACK_OBJECT g_cb;

static NDIS_OID g_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_GEN_STATISTICS, OID_GEN_LINK_PARAMETERS, OID_GEN_INTERRUPT_MODERATION, OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
};

static void LoadMac(ADAPTER *a)
{
    static const WCHAR kn[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\j606fnet\\Parameters";
    UNICODE_STRING us, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE k;
    UCHAR raw[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    ULONG got = 0, seed, i;
    int have = 0;

    RtlInitUnicodeString(&us, kn);
    RtlInitUnicodeString(&vn, L"Mac");
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&k, KEY_READ | KEY_WRITE, &oa, 0, NULL, 0, NULL)))
        k = NULL;
    if (k && NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, raw, sizeof(raw), &got))) {
        PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)raw;
        if (v->Type == REG_BINARY && v->DataLength == 6) {
            NdisMoveMemory(a->mac, v->Data, 6);
            have = 1;
        }
    }
    if (!have) {
        LARGE_INTEGER pc = KeQueryPerformanceCounter(NULL), st;
        KeQuerySystemTime(&st);
        seed = pc.LowPart ^ st.LowPart ^ (ULONG)(ULONG_PTR)a;
        for (i = 0; i < 6; i++)
            a->mac[i] = (UCHAR)(seed = seed * 1103515245u + 12345u, seed >> 16);
        a->mac[0] = (UCHAR)((a->mac[0] & 0xfe) | 2);    // unicast, locally administered
        if (k)
            ZwSetValueKey(k, &vn, 0, REG_BINARY, a->mac, 6);
    }
    if (k)
        ZwClose(k);
}

static void LinkIndicate(ADAPTER *a, int up)
{
    NDIS_LINK_STATE ls;
    NDIS_STATUS_INDICATION si;

    NdisZeroMemory(&ls, sizeof(ls));
    ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState = MediaDuplexStateHalf;
    ls.XmitLinkSpeed = up ? J_LINK : NDIS_LINK_SPEED_UNKNOWN;
    ls.RcvLinkSpeed = up ? J_LINK : NDIS_LINK_SPEED_UNKNOWN;
    ls.PauseFunctions = NdisPauseFunctionsUnsupported;
    NdisZeroMemory(&si, sizeof(si));
    si.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    si.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    si.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    si.SourceHandle = a->h;
    si.StatusCode = NDIS_STATUS_LINK_STATE;
    si.StatusBuffer = &ls;
    si.StatusBufferSize = sizeof(ls);
    NdisMIndicateStatusEx(a->h, &si);
}

static void BusLink(void *ctx, int up)
{
    ADAPTER *a = ctx;

    if (InterlockedExchange(&a->up, up) != up)
        LinkIndicate(a, up);
}

static void BusRx(void *ctx, const UCHAR *f, ULONG n)
{
    ADAPTER *a = ctx;
    PNET_BUFFER nb;

    if (a->paused || n < 14 || n > J_FRAME || !a->nbl) {
        a->rx_nobuf++;
        return;
    }
    NdisMoveMemory(a->rxbuf, f, n);
    nb = NET_BUFFER_LIST_FIRST_NB(a->nbl);
    NET_BUFFER_DATA_LENGTH(nb) = n;
    NET_BUFFER_DATA_OFFSET(nb) = 0;
    NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
    a->nbl->SourceHandle = a->h;
    NET_BUFFER_LIST_STATUS(a->nbl) = NDIS_STATUS_SUCCESS;
    NdisMIndicateReceiveNetBufferLists(a->h, a->nbl, 0, 1,
        NDIS_RECEIVE_FLAGS_RESOURCES | (KeGetCurrentIrql() == DISPATCH_LEVEL ? NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL : 0));
    a->rx_ok++;
}

static BOOLEAN CheckHang(NDIS_HANDLE ctx)
{
    ADAPTER *a = ctx;

    if (!a->bind.tx && g_cb)
        ExNotifyCallback(g_cb, (PVOID)1, &a->bind);
    return FALSE;
}

static NDIS_STATUS Initialize(NDIS_HANDLE h, NDIS_HANDLE drvctx, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    NDIS_MINIPORT_ADAPTER_ATTRIBUTES at;
    NDIS_PM_CAPABILITIES pm;
    NET_BUFFER_LIST_POOL_PARAMETERS pp;
    ADAPTER *a;
    NDIS_STATUS st;

    UNREFERENCED_PARAMETER(drvctx);
    UNREFERENCED_PARAMETER(ip);
    a = NdisAllocateMemoryWithTagPriority(h, sizeof(*a), J_TAG, NormalPoolPriority);
    if (!a)
        return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->h = h;
    a->paused = 1;
    a->lookahead = J_MTU;
    KeInitializeSpinLock(&a->txlock);
    LoadMac(a);

    NdisZeroMemory(&at, sizeof(at));
    at.RegistrationAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    at.RegistrationAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    at.RegistrationAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    at.RegistrationAttributes.MiniportAdapterContext = a;
    at.RegistrationAttributes.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM;
    at.RegistrationAttributes.CheckForHangTimeInSeconds = 2;
    at.RegistrationAttributes.InterfaceType = NdisInterfaceInternal;
    st = NdisMSetMiniportAttributes(h, &at);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;

    NdisZeroMemory(&pp, sizeof(pp));
    pp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pp.fAllocateNetBuffer = TRUE;
    pp.PoolTag = J_TAG;
    a->pool = NdisAllocateNetBufferListPool(h, &pp);
    a->rxbuf = NdisAllocateMemoryWithTagPriority(h, 2048, J_TAG, NormalPoolPriority);
    if (a->pool && a->rxbuf)
        a->mdl = NdisAllocateMdl(h, a->rxbuf, 2048);
    if (a->mdl)
        a->nbl = NdisAllocateNetBufferAndNetBufferList(a->pool, 0, 0, a->mdl, 0, 0);
    if (!a->nbl) {
        st = NDIS_STATUS_RESOURCES;
        goto fail;
    }

    NdisZeroMemory(&pm, sizeof(pm));
    pm.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pm.Header.Revision = NDIS_PM_CAPABILITIES_REVISION_1;
    pm.Header.Size = NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1;
    pm.MinMagicPacketWakeUp = NdisDeviceStateUnspecified;
    pm.MinPatternWakeUp = NdisDeviceStateUnspecified;
    pm.MinLinkChangeWakeUp = NdisDeviceStateUnspecified;

    NdisZeroMemory(&at, sizeof(at));
    at.GeneralAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    at.GeneralAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    at.GeneralAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    at.GeneralAttributes.MediaType = NdisMedium802_3;
    at.GeneralAttributes.PhysicalMediumType = NdisPhysicalMediumUnspecified;
    at.GeneralAttributes.MtuSize = J_MTU;
    at.GeneralAttributes.MaxXmitLinkSpeed = J_LINK;
    at.GeneralAttributes.MaxRcvLinkSpeed = J_LINK;
    at.GeneralAttributes.XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    at.GeneralAttributes.RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    at.GeneralAttributes.MediaConnectState = MediaConnectStateDisconnected;
    at.GeneralAttributes.MediaDuplexState = MediaDuplexStateHalf;
    at.GeneralAttributes.LookaheadSize = J_MTU;
    at.GeneralAttributes.PowerManagementCapabilitiesEx = &pm;
    at.GeneralAttributes.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    at.GeneralAttributes.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST;
    at.GeneralAttributes.MaxMulticastListSize = 32;
    at.GeneralAttributes.MacAddressLength = 6;
    NdisMoveMemory(at.GeneralAttributes.PermanentMacAddress, a->mac, 6);
    NdisMoveMemory(at.GeneralAttributes.CurrentMacAddress, a->mac, 6);
    at.GeneralAttributes.RecvScaleCapabilities = NULL;
    at.GeneralAttributes.AccessType = NET_IF_ACCESS_BROADCAST;
    at.GeneralAttributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    at.GeneralAttributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    at.GeneralAttributes.IfType = IF_TYPE_ETHERNET_CSMACD;
    at.GeneralAttributes.IfConnectorPresent = TRUE;
    at.GeneralAttributes.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED | NDIS_STATISTICS_RCV_OK_SUPPORTED | NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
        NDIS_STATISTICS_RCV_ERROR_SUPPORTED | NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED;
    at.GeneralAttributes.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    at.GeneralAttributes.AutoNegotiationFlags = 0;
    at.GeneralAttributes.SupportedOidList = g_oids;
    at.GeneralAttributes.SupportedOidListLength = sizeof(g_oids);
    st = NdisMSetMiniportAttributes(h, &at);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;

    a->bind.rx = BusRx;
    a->bind.link = BusLink;
    a->bind.ctx = a;
    NdisMoveMemory(a->bind.mac, a->mac, 6);
    return NDIS_STATUS_SUCCESS;
fail:
    if (a->nbl) NdisFreeNetBufferList(a->nbl);
    if (a->mdl) NdisFreeMdl(a->mdl);
    if (a->pool) NdisFreeNetBufferListPool(a->pool);
    if (a->rxbuf) NdisFreeMemoryWithTagPriority(h, a->rxbuf, J_TAG);
    NdisFreeMemoryWithTagPriority(h, a, J_TAG);
    return st;
}

static void Halt(NDIS_HANDLE ctx, NDIS_HALT_ACTION act)
{
    ADAPTER *a = ctx;

    UNREFERENCED_PARAMETER(act);
    if (g_cb)
        ExNotifyCallback(g_cb, (PVOID)0, NULL);         // btbus forgets us before the memory goes away
    NdisFreeNetBufferList(a->nbl);
    NdisFreeMdl(a->mdl);
    NdisFreeNetBufferListPool(a->pool);
    NdisFreeMemoryWithTagPriority(a->h, a->rxbuf, J_TAG);
    NdisFreeMemoryWithTagPriority(a->h, a, J_TAG);
}

static NDIS_STATUS Pause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS p)
{
    UNREFERENCED_PARAMETER(p);
    InterlockedExchange(&((ADAPTER *)ctx)->paused, 1);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS Restart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS p)
{
    UNREFERENCED_PARAMETER(p);
    InterlockedExchange(&((ADAPTER *)ctx)->paused, 0);
    return NDIS_STATUS_SUCCESS;
}

static void Send(NDIS_HANDLE ctx, PNET_BUFFER_LIST list, NDIS_PORT_NUMBER port, ULONG flags)
{
    ADAPTER *a = ctx;
    PNET_BUFFER_LIST l;
    KIRQL irql;
    NDIS_STATUS st = a->paused ? NDIS_STATUS_PAUSED : (!a->up || !a->bind.tx) ? NDIS_STATUS_MEDIA_DISCONNECTED : NDIS_STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(port);
    for (l = list; l; l = NET_BUFFER_LIST_NEXT_NBL(l)) {
        PNET_BUFFER nb;
        NET_BUFFER_LIST_STATUS(l) = st;
        if (st != NDIS_STATUS_SUCCESS)
            continue;
        for (nb = NET_BUFFER_LIST_FIRST_NB(l); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG n = NET_BUFFER_DATA_LENGTH(nb);
            UCHAR *d;
            int r = -1;
            KeAcquireSpinLock(&a->txlock, &irql);
            if (n >= 14 && n <= J_FRAME && (d = NdisGetDataBuffer(nb, n, a->txbuf, 1, 0)) != NULL)
                r = a->bind.tx(d, n);
            KeReleaseSpinLock(&a->txlock, irql);
            if (r) a->tx_bad++; else a->tx_ok++;
        }
    }
    NdisMSendNetBufferListsComplete(a->h, list, NDIS_TEST_SEND_COMPLETE_AT_DISPATCH_LEVEL(flags) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}

static void ReturnNbl(NDIS_HANDLE ctx, PNET_BUFFER_LIST l, ULONG f) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(l); UNREFERENCED_PARAMETER(f); }
static void CancelSend(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }
static void PnpEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT e) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(e); }
static void Shutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION s) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(s); }
static void CancelOid(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }
static NDIS_STATUS Reset(NDIS_HANDLE ctx, PBOOLEAN ar) { UNREFERENCED_PARAMETER(ctx); *ar = FALSE; return NDIS_STATUS_SUCCESS; }

static NDIS_STATUS Query(ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG written, PULONG needed)
{
    ULONG u = 0;
    ULONG64 q = 0;
    NDIS_STATISTICS_INFO si;
    const void *src = &u;
    ULONG n = 4;
    static const char vendor[] = "J606F WCN3990 Wi-Fi";

    switch (oid) {
    case OID_GEN_SUPPORTED_LIST: src = g_oids; n = sizeof(g_oids); break;
    case OID_GEN_HARDWARE_STATUS: u = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE: u = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE: u = J_MTU; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE: u = J_FRAME; break;
    case OID_GEN_LINK_SPEED: u = (ULONG)(J_LINK / 100); break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE: u = J_FRAME * 32; break;
    case OID_GEN_VENDOR_ID: u = 0x00ffffff; break;
    case OID_GEN_VENDOR_DESCRIPTION: src = vendor; n = sizeof(vendor); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: u = 0x00010000; break;
    case OID_GEN_DRIVER_VERSION: u = 0x061e; n = 2; break;
    case OID_GEN_CURRENT_PACKET_FILTER: u = a->filter; break;
    case OID_GEN_MAC_OPTIONS: u = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK; break;
    case OID_GEN_MEDIA_CONNECT_STATUS: u = a->up ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: u = 1; break;
    case OID_GEN_XMIT_OK: q = a->tx_ok; src = &q; n = 8; break;
    case OID_GEN_RCV_OK: q = a->rx_ok; src = &q; n = 8; break;
    case OID_GEN_XMIT_ERROR: q = a->tx_bad; src = &q; n = 8; break;
    case OID_GEN_RCV_ERROR: q = a->rx_bad; src = &q; n = 8; break;
    case OID_GEN_RCV_NO_BUFFER: q = a->rx_nobuf; src = &q; n = 8; break;
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS: src = a->mac; n = 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: u = 32; break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: u = 0; break;
    case OID_GEN_STATISTICS:
        NdisZeroMemory(&si, sizeof(si));
        si.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        si.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        si.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        si.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT;
        si.ifHCInUcastPkts = a->rx_ok;
        si.ifHCOutUcastPkts = a->tx_ok;
        src = &si;
        n = sizeof(si);
        break;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
    *needed = n;
    if (len < n) {
        *written = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(buf, src, n);
    *written = n;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS OidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST r)
{
    ADAPTER *a = ctx;

    switch (r->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        return Query(a, r->DATA.QUERY_INFORMATION.Oid, r->DATA.QUERY_INFORMATION.InformationBuffer, r->DATA.QUERY_INFORMATION.InformationBufferLength,
            (PULONG)&r->DATA.QUERY_INFORMATION.BytesWritten, (PULONG)&r->DATA.QUERY_INFORMATION.BytesNeeded);
    case NdisRequestSetInformation:
        switch (r->DATA.SET_INFORMATION.Oid) {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (r->DATA.SET_INFORMATION.InformationBufferLength < 4)
                return NDIS_STATUS_INVALID_LENGTH;
            a->filter = *(ULONG *)r->DATA.SET_INFORMATION.InformationBuffer;
            r->DATA.SET_INFORMATION.BytesRead = 4;
            return NDIS_STATUS_SUCCESS;
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_802_3_MULTICAST_LIST:
        case OID_GEN_INTERRUPT_MODERATION:
        case OID_GEN_LINK_PARAMETERS:
            r->DATA.SET_INFORMATION.BytesRead = r->DATA.SET_INFORMATION.InformationBufferLength;
            return NDIS_STATUS_SUCCESS;
        }
        return NDIS_STATUS_NOT_SUPPORTED;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static void Unload(PDRIVER_OBJECT d)
{
    UNREFERENCED_PARAMETER(d);
    NdisMDeregisterMiniportDriver(g_drv);
    if (g_cb)
        ObDereferenceObject(g_cb);
}

// A crash at load must not become a boot loop: the driver only starts with C:\\j606f\\drv\\net.on (kept) or net.arm (one shot, deleted on open).
static int Armed(const WCHAR *path, ULONG opts)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE h;

    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &io, NULL, 0, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | opts, NULL, 0)))
        return 0;
    ZwClose(h);
    return 1;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT d, PUNICODE_STRING reg)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c;
    UNICODE_STRING n;
    OBJECT_ATTRIBUTES oa;

    if (!Armed(L"\\??\\C:\\j606f\\drv\\net.on", 0) && !Armed(L"\\??\\C:\\j606f\\drv\\net.arm", FILE_DELETE_ON_CLOSE))
        return STATUS_UNSUCCESSFUL;

    RtlInitUnicodeString(&n, L"\\Callback\\J606fWlan");
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
    if (!NT_SUCCESS(ExCreateCallback(&g_cb, &oa, TRUE, TRUE)))
        g_cb = NULL;

    NdisZeroMemory(&c, sizeof(c));
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6;
    c.MinorNdisVersion = 30;
    c.MajorDriverVersion = 1;
    c.MinorDriverVersion = 0;
    c.InitializeHandlerEx = Initialize;
    c.HaltHandlerEx = Halt;
    c.UnloadHandler = Unload;
    c.PauseHandler = Pause;
    c.RestartHandler = Restart;
    c.OidRequestHandler = OidRequest;
    c.SendNetBufferListsHandler = Send;
    c.ReturnNetBufferListsHandler = ReturnNbl;
    c.CancelSendHandler = CancelSend;
    c.CheckForHangHandlerEx = CheckHang;
    c.ResetHandlerEx = Reset;
    c.DevicePnPEventNotifyHandler = PnpEvent;
    c.ShutdownHandlerEx = Shutdown;
    c.CancelOidRequestHandler = CancelOid;
    return NdisMRegisterMiniportDriver(d, reg, NULL, &c, &g_drv);
}
