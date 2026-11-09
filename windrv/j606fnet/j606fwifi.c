// J606F Wi-Fi: NDIS 6.30 Native 802.11 (ExtSTA) miniport, the "real Wi-Fi adapter" successor of j606fnet.
//  WlanSvc drives scans, connections and WPA2; btbus does the radio work over \\Callback\\J606fWlan (op 2). Data crosses as Ethernet frames, converted here to and from 802.11. OIDs are logged to C:\j606f\out\j606fwifi.txt (ids and lengths only).
//  Starts only with C:\j606f\drv\wifi.arm (one shot, deleted on open) or wifi.on (kept), so a crash at load cannot become a boot loop.
#define NDIS_MINIPORT_DRIVER 1
#define NDIS630_MINIPORT 1
#define NDIS_WDM 1
#include <ndis.h>
#include <windot11.h>

#define J_MTU   1500
#define J_LINK  (433300000ULL)
#define J_TAG   'W60J'

#define BSS_MAX  40
#define BSS_IE   1024
#define BSS_AGE  (120ULL * 10000000ULL)

typedef struct {
    UCHAR mac[6];
    void (*rx)(void *ctx, const UCHAR *f, ULONG n);
    void (*link)(void *ctx, int up);
    void *ctx;
    int (*tx)(const UCHAR *f, ULONG n);
    void (*bss)(void *ctx, const UCHAR *f, ULONG n, ULONG chan, LONG rssi);
    void (*scan_done)(void *ctx);
    void (*assoc)(void *ctx, ULONG status, const UCHAR *req, ULONG reqn, const UCHAR *resp, ULONG respn);
    void (*lost)(void *ctx, ULONG reason);
    int (*scan)(void);
    int (*connect)(const UCHAR *f, ULONG n, ULONG chan, ULONG rsn);
    void (*disconnect)(void);
    void (*key)(ULONG idx, ULONG group, const UCHAR *k, ULONG n);
    ULONG mbps;
    LONG rssi;
} J606F_WIFI_BIND;

typedef struct {
    UCHAR bssid[6];
    USHORT cap, bint;
    ULONG chan, ielen, used;
    LONG rssi;
    ULONG64 ts, host;
    UCHAR ies[BSS_IE];
} BSS;

typedef struct {
    NDIS_HANDLE h;
    NDIS_HANDLE timer, bind_timer, conn_timer;
    NDIS_HANDLE pool;
    PNET_BUFFER_LIST nbl;
    PMDL mdl;
    UCHAR *rxbuf;
    UCHAR mac[6];
    volatile LONG paused;
    ULONG scan_pending;
    PVOID scan_id;
    NDIS_HANDLE scan_handle;
    J606F_WIFI_BIND bind;
    BSS *bss;
    KSPIN_LOCK lock, txlock;
    DOT11_SSID ssid;
    UCHAR want_bssid[6];
    ULONG auth, ucipher, mcipher;
    volatile LONG assoc;                // associated: data may flow
    ULONG try_set;
    LARGE_INTEGER try_t;
    UCHAR bssid[6];
    ULONG freq, phy, seq;
    LONG rssi;
    UCHAR frame[36 + BSS_IE];
    ULONG64 qt;
    UCHAR assocbuf[sizeof(DOT11_ASSOCIATION_COMPLETION_PARAMETERS) + 900 + 8];
    UCHAR txbuf[2400], eth[1600];
    DOT11_EXTSTA_RECV_CONTEXT rc;
} ADAPTER;

static NDIS_HANDLE g_drv;
static PCALLBACK_OBJECT g_cb;

static const UCHAR g_ofdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
static const UCHAR g_erp[]  = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
static DOT11_PHY_ATTRIBUTES g_phy[4];
static DOT11_EXTSTA_ATTRIBUTES g_sta;
static DOT11_AUTH_CIPHER_PAIR g_ucast[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE }, { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP } };
static DOT11_AUTH_CIPHER_PAIR g_mcast[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE }, { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP } };

// Simple 4-byte OIDs: readable and writable, the value is remembered and has no effect yet.
static struct { NDIS_OID oid; ULONG v; } g_u4[] = {
    { OID_DOT11_MPDU_MAX_LENGTH, 2346 }, { OID_DOT11_ATIM_WINDOW, 0 },
    { OID_DOT11_BEACON_PERIOD, 100 }, { OID_DOT11_DTIM_PERIOD, 1 }, { OID_DOT11_RTS_THRESHOLD, 2347 }, { OID_DOT11_FRAGMENTATION_THRESHOLD, 2346 },
    { OID_DOT11_CURRENT_REG_DOMAIN, 0 }, { OID_DOT11_AUTO_CONFIG_ENABLED, 3 }, { OID_DOT11_DESIRED_BSS_TYPE, dot11_BSS_type_infrastructure },
    { OID_DOT11_CURRENT_PHY_ID, 0 }, { OID_DOT11_UNREACHABLE_DETECTION_THRESHOLD, 5000 }, { OID_DOT11_MEDIA_STREAMING_ENABLED, 0 },
    { OID_DOT11_EXCLUDE_UNENCRYPTED, 0 }, { OID_DOT11_UNICAST_USE_GROUP_ENABLED, 0 }, { OID_DOT11_SAFE_MODE_ENABLED, 0 }, { OID_DOT11_HIDDEN_NETWORK_ENABLED, 0 },
    { OID_DOT11_CIPHER_DEFAULT_KEY_ID, 0 }, { OID_DOT11_CURRENT_PACKET_FILTER, 0 }, { OID_GEN_CURRENT_PACKET_FILTER, 0 }, { OID_DOT11_SAFE_MODE_HT_ENABLED, 0 },
};

static NDIS_OID g_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS, OID_GEN_STATISTICS,
    OID_DOT11_MPDU_MAX_LENGTH, OID_DOT11_OPERATION_MODE_CAPABILITY, OID_DOT11_CURRENT_OPERATION_MODE, OID_DOT11_CURRENT_PACKET_FILTER, OID_DOT11_ATIM_WINDOW, OID_DOT11_SCAN_REQUEST,
    OID_DOT11_RESET_REQUEST, OID_DOT11_NIC_POWER_STATE, OID_DOT11_STATION_ID, OID_DOT11_BEACON_PERIOD, OID_DOT11_DTIM_PERIOD, OID_DOT11_MAC_ADDRESS, OID_DOT11_RTS_THRESHOLD,
    OID_DOT11_FRAGMENTATION_THRESHOLD, OID_DOT11_SUPPORTED_PHY_TYPES, OID_DOT11_CURRENT_REG_DOMAIN, OID_DOT11_OPERATIONAL_RATE_SET, OID_DOT11_CURRENT_ADDRESS, OID_DOT11_PERMANENT_ADDRESS,
    OID_DOT11_MULTICAST_LIST, OID_DOT11_MAXIMUM_LIST_SIZE,
    OID_DOT11_AUTO_CONFIG_ENABLED, OID_DOT11_ENUM_BSS_LIST, OID_DOT11_FLUSH_BSS_LIST, OID_DOT11_POWER_MGMT_REQUEST, OID_DOT11_DESIRED_SSID_LIST, OID_DOT11_EXCLUDED_MAC_ADDRESS_LIST,
    OID_DOT11_DESIRED_BSSID_LIST, OID_DOT11_DESIRED_BSS_TYPE, OID_DOT11_CONNECT_REQUEST, OID_DOT11_EXCLUDE_UNENCRYPTED, OID_DOT11_STATISTICS, OID_DOT11_PRIVACY_EXEMPTION_LIST,
    OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM, OID_DOT11_SUPPORTED_UNICAST_ALGORITHM_PAIR, OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM, OID_DOT11_SUPPORTED_MULTICAST_ALGORITHM_PAIR,
    OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM, OID_DOT11_CIPHER_DEFAULT_KEY_ID, OID_DOT11_CIPHER_DEFAULT_KEY, OID_DOT11_CIPHER_KEY_MAPPING_KEY, OID_DOT11_ENUM_ASSOCIATION_INFO,
    OID_DOT11_DISCONNECT_REQUEST, OID_DOT11_UNICAST_USE_GROUP_ENABLED, OID_DOT11_HARDWARE_PHY_STATE, OID_DOT11_DESIRED_PHY_LIST, OID_DOT11_CURRENT_PHY_ID, OID_DOT11_MEDIA_STREAMING_ENABLED,
    OID_DOT11_UNREACHABLE_DETECTION_THRESHOLD, OID_DOT11_ACTIVE_PHY_LIST, OID_DOT11_EXTSTA_CAPABILITY, OID_DOT11_DATA_RATE_MAPPING_TABLE, OID_DOT11_SUPPORTED_COUNTRY_OR_REGION_STRING,
    OID_DOT11_DESIRED_COUNTRY_OR_REGION_STRING, OID_DOT11_IBSS_PARAMS, OID_DOT11_SAFE_MODE_ENABLED, OID_DOT11_HIDDEN_NETWORK_ENABLED, OID_DOT11_ASSOCIATION_PARAMS, OID_DOT11_SAFE_MODE_HT_ENABLED,
    OID_DOT11_PMKID_LIST, OID_DOT11_PORT_STATE_NOTIFICATION,
};

// Append one line to the log. Only at PASSIVE_LEVEL; ids and lengths only, never frame or list contents.
static void Log(const char *s)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE f;
    ULONG n = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    while (s[n]) n++;
    RtlInitUnicodeString(&us, L"\\??\\C:\\j606f\\out\\j606fwifi.txt");
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&f, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        ZwWriteFile(f, NULL, NULL, NULL, &io, (PVOID)s, n, NULL, NULL);
        ZwClose(f);
    }
}

static void Hex(char *p, ULONG v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
}

static void LogOid(char kind, NDIS_OID oid, ULONG len, NDIS_STATUS st)
{
    char l[] = "? oid xxxxxxxx len xxxxxxxx st xxxxxxxx\r\n";
    l[0] = kind;
    Hex(l + 6, oid);
    Hex(l + 19, len);
    Hex(l + 31, (ULONG)st);
    Log(l);
}

static void LoadMac(ADAPTER *a)
{
    UNICODE_STRING us, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE k;
    UCHAR raw[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    ULONG got = 0, seed, i;
    int have = 0;

    RtlInitUnicodeString(&us, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\j606fnet\\Parameters");
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
        a->mac[0] = (UCHAR)((a->mac[0] & 0xfe) | 2);
        if (k)
            ZwSetValueKey(k, &vn, 0, REG_BINARY, a->mac, 6);
    }
    if (k)
        ZwClose(k);
}

static void Hdr(PNDIS_OBJECT_HEADER h, UCHAR rev, USHORT size)
{
    h->Type = NDIS_OBJECT_TYPE_DEFAULT;
    h->Revision = rev;
    h->Size = size;
}

static void FillPhy(DOT11_PHY_ATTRIBUTES *p, DOT11_PHY_TYPE t, const UCHAR *r, ULONG n)
{
    ULONG i;

    NdisZeroMemory(p, sizeof(*p));
    Hdr(&p->Header, DOT11_PHY_ATTRIBUTES_REVISION_1, sizeof(*p));
    p->PhyType = t;
    p->bHardwarePhyState = TRUE;
    p->bSoftwarePhyState = TRUE;
    p->uMPDUMaxLength = 2346;
    p->TempType = dot11_temp_type_1;
    p->DiversitySupport = dot11_diversity_support_notsupported;
    p->uNumberSupportedPowerLevels = 1;
    p->TxPowerLevels[0] = 100;
    p->uNumDataRateMappingEntries = n;
    for (i = 0; i < n; i++) {
        p->DataRateMappingEntries[i].ucDataRateIndex = r[i];
        p->DataRateMappingEntries[i].usDataRateValue = r[i];
        p->SupportedDataRatesValue.ucSupportedTxDataRatesValue[i] = r[i];
        p->SupportedDataRatesValue.ucSupportedRxDataRatesValue[i] = r[i];
    }
}

static void FillAttributes(void)
{
    FillPhy(&g_phy[0], dot11_phy_type_erp, g_erp, sizeof(g_erp));
    g_phy[0].ERPAttributes.bShortSlotTimeOptionImplemented = TRUE;
    FillPhy(&g_phy[1], dot11_phy_type_ofdm, g_ofdm, sizeof(g_ofdm));
    FillPhy(&g_phy[2], dot11_phy_type_ht, g_ofdm, sizeof(g_ofdm));
    FillPhy(&g_phy[3], dot11_phy_type_vht, g_ofdm, sizeof(g_ofdm));
    g_phy[1].OFDMAttributes.uFrequencyBandsSupported = DOT11_FREQUENCY_BANDS_LOWER | DOT11_FREQUENCY_BANDS_MIDDLE | DOT11_FREQUENCY_BANDS_UPPER;

    NdisZeroMemory(&g_sta, sizeof(g_sta));
    Hdr(&g_sta.Header, DOT11_EXTSTA_ATTRIBUTES_REVISION_2, (USHORT)RTL_SIZEOF_THROUGH_FIELD(DOT11_EXTSTA_ATTRIBUTES, pAdhocSupportedMcastAlgoPairs));
    g_sta.uScanSSIDListSize = 4;
    g_sta.uDesiredBSSIDListSize = 8;
    g_sta.uDesiredSSIDListSize = 1;
    g_sta.uExcludedMacAddressListSize = 4;
    g_sta.uPrivacyExemptionListSize = 32;
    g_sta.uKeyMappingTableSize = 32;
    g_sta.uDefaultKeyTableSize = 4;
    g_sta.uWEPKeyValueMaxLength = 13;
    g_sta.uPMKIDCacheSize = 3;
    g_sta.uMaxNumPerSTADefaultKeyTables = 32;
    g_sta.uInfraNumSupportedUcastAlgoPairs = RTL_NUMBER_OF(g_ucast);
    g_sta.pInfraSupportedUcastAlgoPairs = g_ucast;
    g_sta.uInfraNumSupportedMcastAlgoPairs = RTL_NUMBER_OF(g_mcast);
    g_sta.pInfraSupportedMcastAlgoPairs = g_mcast;
}

static void Ind(ADAPTER *a, NDIS_STATUS code, PVOID buf, ULONG n, PVOID id, NDIS_HANDLE dest)
{
    NDIS_STATUS_INDICATION si;

    NdisZeroMemory(&si, sizeof(si));
    si.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    si.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    si.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    si.SourceHandle = a->h;
    si.StatusCode = code;
    si.RequestId = id;
    si.DestinationHandle = dest;
    si.StatusBuffer = buf;
    si.StatusBufferSize = n;
    NdisMIndicateStatusEx(a->h, &si);
}

// PHY index of a network: 3 = VHT, 2 = HT, otherwise ERP (2.4 GHz) or OFDM (5 GHz).
static ULONG PhyOfIes(const UCHAR *ie, ULONG n, ULONG freq)
{
    ULONG o = 0, phy = freq < 3000 ? 0 : 1;

    while (o + 2 <= n && o + 2 + ie[o + 1] <= n) {
        if (ie[o] == 191 && freq >= 3000) return 3;
        if (ie[o] == 45) phy = 2;
        o += 2 + ie[o + 1];
    }
    return phy;
}

// Windows draws the signal bars from this status, not from the scan list.
static void IndicateQuality(ADAPTER *a)
{
    UCHAR b[sizeof(DOT11_LINK_QUALITY_PARAMETERS) + sizeof(DOT11_LINK_QUALITY_ENTRY)];
    DOT11_LINK_QUALITY_PARAMETERS *p = (PVOID)b;
    DOT11_LINK_QUALITY_ENTRY *e = (PVOID)(p + 1);
    LONG r = a->rssi;

    NdisZeroMemory(b, sizeof(b));
    Hdr(&p->Header, DOT11_LINK_QUALITY_PARAMETERS_REVISION_1, sizeof(*p));
    p->uLinkQualityListSize = sizeof(*e);
    p->uLinkQualityListOffset = sizeof(*p);
    NdisMoveMemory(e->PeerMacAddr, a->bssid, 6);
    e->ucLinkQuality = r <= -100 ? 0 : r >= -50 ? 100 : (UCHAR)(2 * (r + 100));
    Ind(a, NDIS_STATUS_DOT11_LINK_QUALITY, b, sizeof(b), NULL, NULL);
}

static void LinkIndicate(ADAPTER *a, int up)
{
    NDIS_LINK_STATE ls;

    NdisZeroMemory(&ls, sizeof(ls));
    ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState = MediaDuplexStateFull;
    ls.XmitLinkSpeed = up ? (a->bind.mbps ? a->bind.mbps * 1000000ULL : J_LINK) : NDIS_LINK_SPEED_UNKNOWN;
    ls.RcvLinkSpeed = ls.XmitLinkSpeed;
    ls.PauseFunctions = NdisPauseFunctionsUnsupported;
    Ind(a, NDIS_STATUS_LINK_STATE, &ls, sizeof(ls), NULL, NULL);
}

static void ConfirmScan(ADAPTER *a)
{
    ULONG status = DOT11_STATUS_SUCCESS;

    if (InterlockedExchange((LONG *)&a->scan_pending, 0))
        Ind(a, NDIS_STATUS_DOT11_SCAN_CONFIRM, &status, sizeof(status), a->scan_id, a->scan_handle);
}

static ULONG ChanFreq(ULONG c) { return c == 14 ? 2484 : c < 14 ? 2407 + 5 * c : 5000 + 5 * c; }

static void BusBss(void *ctx, const UCHAR *f, ULONG n, ULONG chan, LONG rssi)
{
    ADAPTER *a = ctx;
    KIRQL k;
    ULONG i, slot = BSS_MAX, old = 0;
    ULONG64 oldest = ~0ULL;
    LARGE_INTEGER now;

    if (n < 36 || !a->bss)
        return;
    KeQuerySystemTime(&now);
    KeAcquireSpinLock(&a->lock, &k);
    for (i = 0; i < BSS_MAX; i++) {
        BSS *b = &a->bss[i];
        if (b->used && RtlEqualMemory(b->bssid, f + 16, 6)) { slot = i; break; }
        if (!b->used) { if (slot == BSS_MAX) slot = i; }
        else if (b->host < oldest) { oldest = b->host; old = i; }
    }
    if (slot == BSS_MAX)
        slot = old;
    {
        BSS *b = &a->bss[slot];
        ULONG ie = n - 36;
        if (ie > BSS_IE) ie = BSS_IE;
        b->used = 1;
        NdisMoveMemory(b->bssid, f + 16, 6);
        NdisMoveMemory(&b->ts, f + 24, 8);
        b->bint = f[32] | ((USHORT)f[33] << 8);
        b->cap = f[34] | ((USHORT)f[35] << 8);
        b->chan = chan;
        b->rssi = rssi;
        b->host = (ULONG64)now.QuadPart;
        b->ielen = ie;
        NdisMoveMemory(b->ies, f + 36, ie);
    }
    KeReleaseSpinLock(&a->lock, k);
}

static void BusScanDone(void *ctx)
{
    ADAPTER *a = ctx;

    NdisCancelTimerObject(a->timer);
    ConfirmScan(a);
}

static void ConnDone(ADAPTER *a, ULONG status)
{
    DOT11_CONNECTION_COMPLETION_PARAMETERS c;

    NdisZeroMemory(&c, sizeof(c));
    Hdr(&c.Header, DOT11_CONNECTION_COMPLETION_PARAMETERS_REVISION_1, sizeof(c));
    c.uStatus = status;
    Ind(a, NDIS_STATUS_DOT11_CONNECTION_COMPLETION, &c, sizeof(c), NULL, NULL);
}

static void BusAssoc(void *ctx, ULONG status, const UCHAR *req, ULONG reqn, const UCHAR *resp, ULONG respn)
{
    ADAPTER *a = ctx;
    DOT11_ASSOCIATION_COMPLETION_PARAMETERS *p = (PVOID)a->assocbuf;
    ULONG o = sizeof(*p), ie = 0, i, mc = a->mcipher;
    UCHAR *d = a->assocbuf;
    KIRQL k;

    NdisZeroMemory(p, sizeof(*p));
    Hdr(&p->Header, DOT11_ASSOCIATION_COMPLETION_PARAMETERS_REVISION_1, (USHORT)RTL_SIZEOF_THROUGH_FIELD(DOT11_ASSOCIATION_COMPLETION_PARAMETERS, uEncapTableSize));
    o = RTL_SIZEOF_THROUGH_FIELD(DOT11_ASSOCIATION_COMPLETION_PARAMETERS, uEncapTableSize);
    NdisMoveMemory(p->MacAddr, a->bssid, 6);
    p->uStatus = status ? DOT11_ASSOC_STATUS_FAILURE : DOT11_ASSOC_STATUS_SUCCESS;
    if (!status) {
        if (reqn > 300) reqn = 300;
        if (respn > 300) respn = 300;
        p->uAssocReqOffset = o; p->uAssocReqSize = reqn;
        NdisMoveMemory(d + o, req, reqn); o += reqn;
        p->uAssocRespOffset = o; p->uAssocRespSize = respn;
        NdisMoveMemory(d + o, resp, respn); o += respn;
        KeAcquireSpinLock(&a->lock, &k);            // the beacon body the connection was chosen from: fixed fields + IEs
        for (i = 0; i < BSS_MAX; i++)
            if (a->bss[i].used && RtlEqualMemory(a->bss[i].bssid, a->bssid, 6)) {
                ie = a->bss[i].ielen > 300 ? 300 : a->bss[i].ielen;
                NdisMoveMemory(d + o, &a->bss[i].ts, 8);
                d[o + 8] = (UCHAR)a->bss[i].bint; d[o + 9] = (UCHAR)(a->bss[i].bint >> 8);
                d[o + 10] = (UCHAR)a->bss[i].cap; d[o + 11] = (UCHAR)(a->bss[i].cap >> 8);
                NdisMoveMemory(d + o + 12, a->bss[i].ies, ie);
                {
                    ULONG q = 0;                    // the cipher pair really in use comes from the network's own RSN element, not from what Windows allowed
                    while (q + 2 <= a->bss[i].ielen && q + 2 + a->bss[i].ies[q + 1] <= a->bss[i].ielen) {
                        if (a->bss[i].ies[q] == 48 && a->bss[i].ies[q + 1] >= 8) {
                            UCHAR g = a->bss[i].ies[q + 7];
                            mc = g == 4 ? DOT11_CIPHER_ALGO_CCMP : g == 2 ? DOT11_CIPHER_ALGO_TKIP : DOT11_CIPHER_ALGO_NONE;
                            break;
                        }
                        q += 2 + a->bss[i].ies[q + 1];
                    }
                }
                break;
            }
        KeReleaseSpinLock(&a->lock, k);
        p->uBeaconOffset = o; p->uBeaconSize = ie + 12;
        o += ie + 12;
        p->AuthAlgo = a->auth;
        p->UnicastCipher = a->ucipher;
        p->MulticastCipher = mc;
        p->uActivePhyListOffset = o; p->uActivePhyListSize = 4;
        *(ULONG UNALIGNED *)(d + o) = a->phy; o += 4;
        p->bFourAddressSupported = FALSE;
        p->bPortAuthorized = FALSE;
        p->DSInfo = DOT11_DS_UNKNOWN;
        InterlockedExchange(&a->assoc, 1);
    }
    {
        char l[] = "assoc status x auth xx uc xx mc xx\r\n";
        l[13] = status ? '1' : '0';
        l[20] = "0123456789abcdef"[(a->auth >> 4) & 15]; l[21] = "0123456789abcdef"[a->auth & 15];
        l[26] = "0123456789abcdef"[(a->ucipher >> 4) & 15]; l[27] = "0123456789abcdef"[a->ucipher & 15];
        l[32] = "0123456789abcdef"[(mc >> 4) & 15]; l[33] = "0123456789abcdef"[mc & 15];
        Log(l);
    }
    Ind(a, NDIS_STATUS_DOT11_ASSOCIATION_COMPLETION, p, o, NULL, NULL);
    if (!status)
        LinkIndicate(a, 1);
    ConnDone(a, status ? DOT11_CONNECTION_STATUS_FAILURE : DOT11_CONNECTION_STATUS_SUCCESS);
}

static void Disassoc(ADAPTER *a, ULONG reason)
{
    DOT11_DISASSOCIATION_PARAMETERS d;

    if (!InterlockedExchange(&a->assoc, 0))
        return;
    NdisZeroMemory(&d, sizeof(d));
    Hdr(&d.Header, DOT11_DISASSOCIATION_PARAMETERS_REVISION_1, sizeof(d));
    NdisMoveMemory(d.MacAddr, a->bssid, 6);
    d.uReason = reason;
    LinkIndicate(a, 0);
    Ind(a, NDIS_STATUS_DOT11_DISASSOCIATION, &d, sizeof(d), NULL, NULL);
}

static void BusLost(void *ctx, ULONG reason)
{
    UNREFERENCED_PARAMETER(reason);
    Disassoc(ctx, DOT11_DISASSOC_REASON_PEER_DEAUTHENTICATED);
}

// Ethernet from btbus -> 802.11 data frame with LLC/SNAP, as Native Wi-Fi expects.
static void BusRx(void *ctx, const UCHAR *f, ULONG n)
{
    ADAPTER *a = ctx;
    PNET_BUFFER nb;
    UCHAR *p = a->rxbuf;
    ULONG len;

    if (a->paused || n < 14 || n > 1514 || !a->nbl || !a->assoc)
        return;
    {
        ULONG64 t = KeQueryInterruptTime();
        if (t - a->qt > 20000000) {                 // every 2 s
            a->qt = t;
            if (a->bind.rssi)
                a->rssi = a->bind.rssi;
            IndicateQuality(a);
        }
    }
    NdisZeroMemory(p, 32);
    p[0] = 0x08;
    p[1] = 0x02;                            // FromDS
    NdisMoveMemory(p + 4, f, 6);            // addr1 = DA
    NdisMoveMemory(p + 10, a->bssid, 6);    // addr2 = BSSID
    NdisMoveMemory(p + 16, f + 6, 6);       // addr3 = SA
    a->seq = (a->seq + 1) & 0xfff;
    p[22] = (UCHAR)(a->seq << 4);
    p[23] = (UCHAR)(a->seq >> 4);
    p[24] = 0xaa; p[25] = 0xaa; p[26] = 0x03;
    p[30] = f[12];
    p[31] = f[13];
    NdisMoveMemory(p + 32, f + 14, n - 14);
    len = 32 + n - 14;
    nb = NET_BUFFER_LIST_FIRST_NB(a->nbl);
    NET_BUFFER_DATA_LENGTH(nb) = len;
    NET_BUFFER_DATA_OFFSET(nb) = 0;
    NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
    NdisZeroMemory(&a->rc, sizeof(a->rc));
    Hdr(&a->rc.Header, DOT11_EXTSTA_RECV_CONTEXT_REVISION_1, sizeof(a->rc));
    a->rc.uPhyId = a->phy;
    a->rc.uChCenterFrequency = a->freq;
    a->rc.usNumberOfMPDUsReceived = 1;
    a->rc.lRSSI = a->rssi;
    a->rc.ucDataRate = 108;
    NET_BUFFER_LIST_INFO(a->nbl, MediaSpecificInformation) = &a->rc;
    a->nbl->SourceHandle = a->h;
    NET_BUFFER_LIST_STATUS(a->nbl) = NDIS_STATUS_SUCCESS;
    NdisMIndicateReceiveNetBufferLists(a->h, a->nbl, 0, 1, NDIS_RECEIVE_FLAGS_RESOURCES | (KeGetCurrentIrql() == DISPATCH_LEVEL ? NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL : 0));
}

static void BusLink(void *ctx, int up) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(up); }

// Timer function: connect to the best known BSS of the requested network.
static void DoConnect(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    ADAPTER *a = ctx;
    DOT11_CONNECTION_START_PARAMETERS cs;
    DOT11_ASSOCIATION_START_PARAMETERS as;
    KIRQL k;
    ULONG i, n = 0, chan = 0, best = BSS_MAX, rsn = a->auth == DOT11_AUTH_ALGO_RSNA_PSK;
    LONG top = -1000;

    UNREFERENCED_PARAMETER(s1); UNREFERENCED_PARAMETER(s2); UNREFERENCED_PARAMETER(s3);
    NdisZeroMemory(&cs, sizeof(cs));
    Hdr(&cs.Header, DOT11_CONNECTION_START_PARAMETERS_REVISION_1, sizeof(cs));
    cs.BSSType = dot11_BSS_type_infrastructure;
    Ind(a, NDIS_STATUS_DOT11_CONNECTION_START, &cs, sizeof(cs), NULL, NULL);
    KeAcquireSpinLock(&a->lock, &k);
    for (i = 0; i < BSS_MAX; i++) {
        BSS *b = &a->bss[i];
        ULONG o = 0, match = 0;
        if (!b->used || (b->cap & 1) == 0)
            continue;
        while (o + 2 <= b->ielen && o + 2 + b->ies[o + 1] <= b->ielen) {
            if (b->ies[o] == 0) {
                match = b->ies[o + 1] == a->ssid.uSSIDLength && RtlEqualMemory(b->ies + o + 2, a->ssid.ucSSID, a->ssid.uSSIDLength);
                break;
            }
            o += 2 + b->ies[o + 1];
        }
        if (match && b->rssi > top) { top = b->rssi; best = i; }
    }
    if (best != BSS_MAX) {
        BSS *b = &a->bss[best];
        NdisZeroMemory(a->frame, 36);
        a->frame[0] = 0x80;
        NdisMoveMemory(a->frame + 10, b->bssid, 6);
        NdisMoveMemory(a->frame + 16, b->bssid, 6);
        NdisMoveMemory(a->frame + 24, &b->ts, 8);
        a->frame[32] = (UCHAR)b->bint; a->frame[33] = (UCHAR)(b->bint >> 8);
        a->frame[34] = (UCHAR)b->cap; a->frame[35] = (UCHAR)(b->cap >> 8);
        NdisMoveMemory(a->frame + 36, b->ies, b->ielen);
        n = 36 + b->ielen;
        chan = b->chan;
        NdisMoveMemory(a->bssid, b->bssid, 6);
        a->freq = ChanFreq(chan);
        a->phy = PhyOfIes(b->ies, b->ielen, a->freq);
        a->rssi = b->rssi;
    }
    KeReleaseSpinLock(&a->lock, k);
    if (best == BSS_MAX || !a->bind.connect) {
        ConnDone(a, DOT11_CONNECTION_STATUS_CANDIDATE_LIST_EXHAUSTED);
        return;
    }
    NdisZeroMemory(&as, sizeof(as));
    Hdr(&as.Header, DOT11_ASSOCIATION_START_PARAMETERS_REVISION_1, sizeof(as));
    NdisMoveMemory(as.MacAddr, a->bssid, 6);
    as.SSID = a->ssid;
    Ind(a, NDIS_STATUS_DOT11_ASSOCIATION_START, &as, sizeof(as), NULL, NULL);
    if (a->bind.connect(a->frame, n, chan, rsn))
        BusAssoc(a, 1, NULL, 0, NULL, 0);
}

static void TryBind(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    ADAPTER *a = ctx;
    LARGE_INTEGER due;

    UNREFERENCED_PARAMETER(s1); UNREFERENCED_PARAMETER(s2); UNREFERENCED_PARAMETER(s3);
    if (!a->bind.tx && g_cb)
        ExNotifyCallback(g_cb, (PVOID)2, &a->bind);
    if (!a->bind.tx) {
        due.QuadPart = -10000000;
        NdisSetTimerObject(a->bind_timer, due, 0, NULL);
    }
}

static void ScanDone(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    UNREFERENCED_PARAMETER(s1); UNREFERENCED_PARAMETER(s2); UNREFERENCED_PARAMETER(s3);
    ConfirmScan(ctx);
}

static NDIS_STATUS Initialize(NDIS_HANDLE h, NDIS_HANDLE drvctx, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    NDIS_MINIPORT_ADAPTER_ATTRIBUTES at;
    NDIS_PM_CAPABILITIES pm;
    NDIS_TIMER_CHARACTERISTICS tc;
    ADAPTER *a;
    NDIS_STATUS st;

    UNREFERENCED_PARAMETER(drvctx); UNREFERENCED_PARAMETER(ip);
    Log("initialize\r\n");
    a = NdisAllocateMemoryWithTagPriority(h, sizeof(*a), J_TAG, NormalPoolPriority);
    if (!a)
        return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->h = h;
    a->paused = 1;
    a->auth = DOT11_AUTH_ALGO_80211_OPEN;
    a->ucipher = a->mcipher = DOT11_CIPHER_ALGO_NONE;
    KeInitializeSpinLock(&a->lock);
    KeInitializeSpinLock(&a->txlock);
    LoadMac(a);
    FillAttributes();

    NdisZeroMemory(&at, sizeof(at));
    at.RegistrationAttributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    at.RegistrationAttributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    at.RegistrationAttributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    at.RegistrationAttributes.MiniportAdapterContext = a;
    at.RegistrationAttributes.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM;
    at.RegistrationAttributes.CheckForHangTimeInSeconds = 0;
    at.RegistrationAttributes.InterfaceType = NdisInterfaceInternal;
    st = NdisMSetMiniportAttributes(h, &at);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;

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
    at.GeneralAttributes.MediaType = NdisMediumNative802_11;
    at.GeneralAttributes.PhysicalMediumType = NdisPhysicalMediumNative802_11;
    at.GeneralAttributes.MtuSize = J_MTU;
    at.GeneralAttributes.MaxXmitLinkSpeed = J_LINK;
    at.GeneralAttributes.MaxRcvLinkSpeed = J_LINK;
    at.GeneralAttributes.XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    at.GeneralAttributes.RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    at.GeneralAttributes.MediaConnectState = MediaConnectStateConnected;      // ExtSTA: the port state, not the association, carries the real link state
    at.GeneralAttributes.MediaDuplexState = MediaDuplexStateFull;
    at.GeneralAttributes.LookaheadSize = J_MTU;
    at.GeneralAttributes.PowerManagementCapabilitiesEx = &pm;
    at.GeneralAttributes.MacOptions = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    at.GeneralAttributes.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST;
    at.GeneralAttributes.MaxMulticastListSize = 32;
    at.GeneralAttributes.MacAddressLength = 6;
    NdisMoveMemory(at.GeneralAttributes.PermanentMacAddress, a->mac, 6);
    NdisMoveMemory(at.GeneralAttributes.CurrentMacAddress, a->mac, 6);
    at.GeneralAttributes.AccessType = NET_IF_ACCESS_BROADCAST;
    at.GeneralAttributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    at.GeneralAttributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    at.GeneralAttributes.IfType = IF_TYPE_IEEE80211;
    at.GeneralAttributes.IfConnectorPresent = TRUE;
    at.GeneralAttributes.SupportedStatistics = 0;
    at.GeneralAttributes.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    at.GeneralAttributes.SupportedOidList = g_oids;
    at.GeneralAttributes.SupportedOidListLength = sizeof(g_oids);
    st = NdisMSetMiniportAttributes(h, &at);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;

    NdisZeroMemory(&at, sizeof(at));
    at.Native_802_11_Attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES;
    at.Native_802_11_Attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_802_11_ATTRIBUTES_REVISION_2;
    at.Native_802_11_Attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES_REVISION_2;
    at.Native_802_11_Attributes.OpModeCapability = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
    at.Native_802_11_Attributes.NumOfTXBuffers = 32;
    at.Native_802_11_Attributes.NumOfRXBuffers = 32;
    at.Native_802_11_Attributes.MultiDomainCapabilityImplemented = FALSE;
    at.Native_802_11_Attributes.NumSupportedPhys = 4;
    at.Native_802_11_Attributes.SupportedPhyAttributes = g_phy;
    at.Native_802_11_Attributes.ExtSTAAttributes = &g_sta;
    st = NdisMSetMiniportAttributes(h, &at);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;

    NdisZeroMemory(&tc, sizeof(tc));
    tc.Header.Type = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    tc.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    tc.Header.Size = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    tc.AllocationTag = J_TAG;
    tc.TimerFunction = ScanDone;
    tc.FunctionContext = a;
    st = NdisAllocateTimerObject(h, &tc, &a->timer);
    if (st != NDIS_STATUS_SUCCESS)
        goto fail;
    tc.TimerFunction = TryBind;
    if (NdisAllocateTimerObject(h, &tc, &a->bind_timer) != NDIS_STATUS_SUCCESS)
        goto fail;
    tc.TimerFunction = DoConnect;
    if (NdisAllocateTimerObject(h, &tc, &a->conn_timer) != NDIS_STATUS_SUCCESS)
        goto fail;
    {
        NET_BUFFER_LIST_POOL_PARAMETERS pp;
        NdisZeroMemory(&pp, sizeof(pp));
        pp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
        pp.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
        pp.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
        pp.fAllocateNetBuffer = TRUE;
        pp.PoolTag = J_TAG;
        a->pool = NdisAllocateNetBufferListPool(h, &pp);
    }
    a->rxbuf = NdisAllocateMemoryWithTagPriority(h, 2048, J_TAG, NormalPoolPriority);
    a->bss = NdisAllocateMemoryWithTagPriority(h, BSS_MAX * sizeof(BSS), J_TAG, NormalPoolPriority);
    if (a->pool && a->rxbuf)
        a->mdl = NdisAllocateMdl(h, a->rxbuf, 2048);
    if (a->mdl)
        a->nbl = NdisAllocateNetBufferAndNetBufferList(a->pool, 0, 0, a->mdl, 0, 0);
    if (!a->nbl || !a->bss) {
        st = NDIS_STATUS_RESOURCES;
        goto fail;
    }
    NdisZeroMemory(a->bss, BSS_MAX * sizeof(BSS));
    a->bind.rx = BusRx;
    a->bind.link = BusLink;
    a->bind.ctx = a;
    a->bind.bss = BusBss;
    a->bind.scan_done = BusScanDone;
    a->bind.assoc = BusAssoc;
    a->bind.lost = BusLost;
    NdisMoveMemory(a->bind.mac, a->mac, 6);
    {
        LARGE_INTEGER due;
        due.QuadPart = -5000000;
        NdisSetTimerObject(a->bind_timer, due, 0, NULL);
    }
    a->try_set = 1;
    KeQuerySystemTime(&a->try_t);
    Log("initialize ok\r\n");
    return NDIS_STATUS_SUCCESS;
fail:
    {
        char l[] = "initialize failed xxxxxxxx\r\n";
        Hex(l + 18, (ULONG)st);
        Log(l);
    }
    NdisFreeMemoryWithTagPriority(h, a, J_TAG);
    return st;
}

static void Halt(NDIS_HANDLE ctx, NDIS_HALT_ACTION act)
{
    ADAPTER *a = ctx;

    UNREFERENCED_PARAMETER(act);
    if (g_cb)
        ExNotifyCallback(g_cb, (PVOID)0, NULL);         // btbus forgets us before the memory goes away
    NdisCancelTimerObject(a->timer);
    NdisCancelTimerObject(a->bind_timer);
    NdisCancelTimerObject(a->conn_timer);
    NdisFreeTimerObject(a->timer);
    NdisFreeTimerObject(a->bind_timer);
    NdisFreeTimerObject(a->conn_timer);
    NdisFreeNetBufferList(a->nbl);
    NdisFreeMdl(a->mdl);
    NdisFreeNetBufferListPool(a->pool);
    NdisFreeMemoryWithTagPriority(a->h, a->rxbuf, J_TAG);
    NdisFreeMemoryWithTagPriority(a->h, a->bss, J_TAG);
    NdisFreeMemoryWithTagPriority(a->h, a, J_TAG);
}

static NDIS_STATUS Pause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS p) { UNREFERENCED_PARAMETER(p); InterlockedExchange(&((ADAPTER *)ctx)->paused, 1); return NDIS_STATUS_SUCCESS; }
static NDIS_STATUS Restart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS p) { UNREFERENCED_PARAMETER(p); InterlockedExchange(&((ADAPTER *)ctx)->paused, 0); return NDIS_STATUS_SUCCESS; }

static void Send(NDIS_HANDLE ctx, PNET_BUFFER_LIST list, NDIS_PORT_NUMBER port, ULONG flags)
{
    ADAPTER *a = ctx;
    PNET_BUFFER_LIST l;
    NDIS_STATUS st = a->paused ? NDIS_STATUS_PAUSED : (!a->assoc || !a->bind.tx) ? NDIS_STATUS_MEDIA_DISCONNECTED : NDIS_STATUS_SUCCESS;
    KIRQL k;

    UNREFERENCED_PARAMETER(port);
    for (l = list; l; l = NET_BUFFER_LIST_NEXT_NBL(l)) {
        PNET_BUFFER nb;
        NET_BUFFER_LIST_STATUS(l) = st;
        if (st != NDIS_STATUS_SUCCESS)
            continue;
        for (nb = NET_BUFFER_LIST_FIRST_NB(l); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG n = NET_BUFFER_DATA_LENGTH(nb), h;
            UCHAR *d;
            KeAcquireSpinLock(&a->txlock, &k);
            if (n >= 32 && n <= sizeof(a->txbuf) && (d = NdisGetDataBuffer(nb, n, a->txbuf, 1, 0)) != NULL && (d[0] & 0x0c) == 0x08) {
                h = (d[0] & 0x80) ? 26 : 24;        // QoS data has two more header bytes
                if (n >= h + 8 && d[h] == 0xaa && d[h + 1] == 0xaa && n - h - 8 + 14 <= 1514) {
                    NdisMoveMemory(a->eth, d + 16, 6);          // addr3 = destination
                    NdisMoveMemory(a->eth + 6, a->mac, 6);
                    a->eth[12] = d[h + 6];
                    a->eth[13] = d[h + 7];
                    NdisMoveMemory(a->eth + 14, d + h + 8, n - h - 8);
                    a->bind.tx(a->eth, 14 + n - h - 8);
                }
            }
            KeReleaseSpinLock(&a->txlock, k);
        }
    }
    NdisMSendNetBufferListsComplete(a->h, list, NDIS_TEST_SEND_AT_DISPATCH_LEVEL(flags) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}

static void ReturnNbl(NDIS_HANDLE ctx, PNET_BUFFER_LIST l, ULONG f) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(l); UNREFERENCED_PARAMETER(f); }
static void CancelSend(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }
static void PnpEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT e) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(e); }
static void Shutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION s) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(s); }
static void CancelOid(NDIS_HANDLE ctx, PVOID id) { UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(id); }
static NDIS_STATUS Reset(NDIS_HANDLE ctx, PBOOLEAN ar) { UNREFERENCED_PARAMETER(ctx); *ar = FALSE; return NDIS_STATUS_SUCCESS; }

// Empty DOT11 list reply: header + uNumOfEntries + uTotalNumOfEntries (revision 1 for all of them).
typedef struct { NDIS_OBJECT_HEADER h; ULONG n, total; } EMPTY_LIST;

static NDIS_STATUS Query(ADAPTER *a, NDIS_OID oid, PVOID buf, ULONG len, PULONG written, PULONG needed)
{
    static const char vendor[] = "J606F WCN3990 Wi-Fi";
    union {
        ULONG u; ULONG64 q;
        NDIS_STATISTICS_INFO gs;
        DOT11_OPERATION_MODE_CAPABILITY cap;
        DOT11_CURRENT_OPERATION_MODE mode;
        EMPTY_LIST empty;
        DOT11_EXTSTA_CAPABILITY sta;
        DOT11_STATISTICS stats;
        struct { NDIS_OBJECT_HEADER h; ULONG n, total, id[4]; } ids;
        struct { ULONG n, total; DOT11_PHY_TYPE t[4]; } types;
        struct { NDIS_OBJECT_HEADER h; ULONG n, total; DOT11_AUTH_CIPHER_PAIR p[2]; } pairs;
        struct { NDIS_OBJECT_HEADER h; ULONG n, total; ULONG a[1]; } algo;
        struct { ULONG n; UCHAR r[12]; } rates;
        struct { NDIS_OBJECT_HEADER h; ULONG n, total; DOT11_SSID s; } ssid;
        struct { NDIS_OBJECT_HEADER h; ULONG n; DOT11_DATA_RATE_MAPPING_ENTRY e[12]; } map;
    } v;
    const void *src = &v;
    ULONG n = 4, i;

    NdisZeroMemory(&v, sizeof(v));
    for (i = 0; i < RTL_NUMBER_OF(g_u4); i++)
        if (g_u4[i].oid == oid) {
            v.u = g_u4[i].v;
            goto reply;
        }
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST: src = g_oids; n = sizeof(g_oids); break;
    case OID_GEN_HARDWARE_STATUS: v.u = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE: v.u = NdisMediumNative802_11; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE: v.u = J_MTU; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE: v.u = 2346; break;
    case OID_GEN_LINK_SPEED: v.u = (ULONG)(J_LINK / 100); break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE: v.u = 2346 * 32; break;
    case OID_GEN_VENDOR_ID: v.u = 0x00ffffff; break;
    case OID_GEN_VENDOR_DESCRIPTION: src = vendor; n = sizeof(vendor); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: v.u = 0x00010000; break;
    case OID_GEN_DRIVER_VERSION: v.u = 0x061e; n = 2; break;
    case OID_GEN_MEDIA_CONNECT_STATUS: v.u = NdisMediaStateConnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: v.u = 1; break;
    case OID_GEN_STATISTICS:
        v.gs.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        v.gs.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        v.gs.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        n = sizeof(v.gs);
        break;
    case OID_DOT11_NIC_POWER_STATE:
    case OID_DOT11_HARDWARE_PHY_STATE: v.u = 1; n = 1; break;
    case OID_DOT11_MAXIMUM_LIST_SIZE: v.u = 32; break;
    case OID_DOT11_OPERATION_MODE_CAPABILITY:
        v.cap.uNumOfTXBuffers = 32; v.cap.uNumOfRXBuffers = 32; v.cap.uOpModeCapability = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
        v.cap.uMajorVersion = 2;
        n = sizeof(v.cap);
        break;
    case OID_DOT11_CURRENT_OPERATION_MODE:
        v.mode.uCurrentOpMode = DOT11_OPERATION_MODE_EXTENSIBLE_STATION;
        n = sizeof(v.mode);
        break;
    case OID_DOT11_MAC_ADDRESS:
    case OID_DOT11_CURRENT_ADDRESS:
    case OID_DOT11_PERMANENT_ADDRESS:
    case OID_DOT11_STATION_ID: src = a->mac; n = 6; break;
    case OID_DOT11_SUPPORTED_PHY_TYPES:
        v.types.n = v.types.total = 4; v.types.t[0] = dot11_phy_type_erp; v.types.t[1] = dot11_phy_type_ofdm; v.types.t[2] = dot11_phy_type_ht; v.types.t[3] = dot11_phy_type_vht;
        n = sizeof(v.types);
        break;
    case OID_DOT11_DESIRED_PHY_LIST:
    case OID_DOT11_ACTIVE_PHY_LIST:
        Hdr(&v.ids.h, DOT11_PHY_ID_LIST_REVISION_1, sizeof(v.ids));
        v.ids.n = v.ids.total = 4; v.ids.id[0] = 0; v.ids.id[1] = 1; v.ids.id[2] = 2; v.ids.id[3] = 3;
        n = sizeof(v.ids);
        break;
    case OID_DOT11_SUPPORTED_UNICAST_ALGORITHM_PAIR:
    case OID_DOT11_SUPPORTED_MULTICAST_ALGORITHM_PAIR:
        Hdr(&v.pairs.h, DOT11_AUTH_CIPHER_PAIR_LIST_REVISION_1, sizeof(v.pairs));
        v.pairs.n = v.pairs.total = 2;
        NdisMoveMemory(v.pairs.p, g_ucast, sizeof(g_ucast));
        n = sizeof(v.pairs);
        break;
    case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
        Hdr(&v.algo.h, DOT11_AUTH_ALGORITHM_LIST_REVISION_1, sizeof(v.algo));
        v.algo.n = v.algo.total = 1; v.algo.a[0] = a->auth;
        n = sizeof(v.algo);
        break;
    case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
    case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
        Hdr(&v.algo.h, DOT11_CIPHER_ALGORITHM_LIST_REVISION_1, sizeof(v.algo));
        v.algo.n = v.algo.total = 1; v.algo.a[0] = oid == OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM ? a->ucipher : a->mcipher;
        n = sizeof(v.algo);
        break;
    case OID_DOT11_OPERATIONAL_RATE_SET:
        v.rates.n = sizeof(g_erp);
        NdisMoveMemory(v.rates.r, g_erp, sizeof(g_erp));
        n = sizeof(v.rates);
        break;
    case OID_DOT11_DATA_RATE_MAPPING_TABLE:
        Hdr(&v.map.h, DOT11_DATA_RATE_MAPPING_TABLE_REVISION_1, sizeof(v.map));
        v.map.n = sizeof(g_erp);
        for (i = 0; i < sizeof(g_erp); i++) { v.map.e[i].ucDataRateIndex = g_erp[i]; v.map.e[i].usDataRateValue = g_erp[i]; }
        n = sizeof(v.map);
        break;
    case OID_DOT11_EXTSTA_CAPABILITY:
        Hdr(&v.sta.Header, DOT11_EXTSTA_CAPABILITY_REVISION_1, sizeof(v.sta));
        v.sta.uScanSSIDListSize = g_sta.uScanSSIDListSize;
        v.sta.uDesiredBSSIDListSize = g_sta.uDesiredBSSIDListSize;
        v.sta.uDesiredSSIDListSize = g_sta.uDesiredSSIDListSize;
        v.sta.uExcludedMacAddressListSize = g_sta.uExcludedMacAddressListSize;
        v.sta.uPrivacyExemptionListSize = g_sta.uPrivacyExemptionListSize;
        v.sta.uKeyMappingTableSize = g_sta.uKeyMappingTableSize;
        v.sta.uDefaultKeyTableSize = g_sta.uDefaultKeyTableSize;
        v.sta.uWEPKeyValueMaxLength = g_sta.uWEPKeyValueMaxLength;
        v.sta.uPMKIDCacheSize = g_sta.uPMKIDCacheSize;
        v.sta.uMaxNumPerSTADefaultKeyTables = g_sta.uMaxNumPerSTADefaultKeyTables;
        n = sizeof(v.sta);
        break;
    case OID_DOT11_STATISTICS:
        Hdr(&v.stats.Header, DOT11_STATISTICS_REVISION_1, sizeof(v.stats));
        n = sizeof(v.stats);
        break;
    case OID_DOT11_DESIRED_SSID_LIST:
        Hdr(&v.ssid.h, DOT11_SSID_LIST_REVISION_1, sizeof(v.ssid));
        v.ssid.n = v.ssid.total = 1;
        v.ssid.s = a->ssid;
        n = sizeof(v.ssid);
        break;
    case OID_DOT11_DESIRED_BSSID_LIST:
    case OID_DOT11_EXCLUDED_MAC_ADDRESS_LIST:
    case OID_DOT11_PRIVACY_EXEMPTION_LIST:
    case OID_DOT11_PMKID_LIST:
    case OID_DOT11_ENUM_ASSOCIATION_INFO:
    case OID_DOT11_SUPPORTED_COUNTRY_OR_REGION_STRING:
    case OID_DOT11_DESIRED_COUNTRY_OR_REGION_STRING:
    case OID_DOT11_MULTICAST_LIST:
        Hdr(&v.empty.h, 1, sizeof(v.empty));
        n = sizeof(v.empty);
        break;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
reply:
    *needed = n;
    if (len < n) {
        *written = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    NdisMoveMemory(buf, src, n);
    *written = n;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS SetOid(ADAPTER *a, PNDIS_OID_REQUEST r, NDIS_OID oid, PVOID buf, ULONG len)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(g_u4); i++)
        if (g_u4[i].oid == oid && len >= 4) {
            g_u4[i].v = *(ULONG *)buf;
            r->DATA.SET_INFORMATION.BytesRead = 4;
            return NDIS_STATUS_SUCCESS;
        }
    r->DATA.SET_INFORMATION.BytesRead = len;
    switch (oid) {
    case OID_DOT11_SCAN_REQUEST:
        a->scan_id = r->RequestId;
        a->scan_handle = r->RequestHandle;
        InterlockedExchange((LONG *)&a->scan_pending, 1);
        {
            LARGE_INTEGER due;
            due.QuadPart = -100000000LL;        // 10 s: confirm anyway if the radio never answers
            NdisSetTimerObject(a->timer, due, 0, NULL);
        }
        if (a->bind.scan)
            a->bind.scan();
        return NDIS_STATUS_INDICATION_REQUIRED;
    case OID_DOT11_DESIRED_SSID_LIST:
        {
            DOT11_SSID_LIST *l = buf;
            if (len >= FIELD_OFFSET(DOT11_SSID_LIST, SSIDs) + sizeof(DOT11_SSID) && l->uNumOfEntries)
                a->ssid = l->SSIDs[0];
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_ENABLED_AUTHENTICATION_ALGORITHM:
        if (len >= sizeof(DOT11_AUTH_ALGORITHM_LIST))
            a->auth = ((DOT11_AUTH_ALGORITHM_LIST *)buf)->AlgorithmIds[0];
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM:
    case OID_DOT11_ENABLED_MULTICAST_CIPHER_ALGORITHM:
        if (len >= sizeof(DOT11_CIPHER_ALGORITHM_LIST)) {
            ULONG c = ((DOT11_CIPHER_ALGORITHM_LIST *)buf)->AlgorithmIds[0];
            if (oid == OID_DOT11_ENABLED_UNICAST_CIPHER_ALGORITHM) a->ucipher = c; else a->mcipher = c;
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_CONNECT_REQUEST:
        {
            LARGE_INTEGER due;
            due.QuadPart = -100000;             // 10 ms: the OID completes first, then the connection indications
            NdisSetTimerObject(a->conn_timer, due, 0, NULL);
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_DISCONNECT_REQUEST:
        if (a->bind.disconnect)
            a->bind.disconnect();
        Disassoc(a, DOT11_DISASSOC_REASON_OS);
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_FLUSH_BSS_LIST:
        {
            KIRQL k;
            KeAcquireSpinLock(&a->lock, &k);
            NdisZeroMemory(a->bss, BSS_MAX * sizeof(BSS));
            KeReleaseSpinLock(&a->lock, k);
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_CIPHER_KEY_MAPPING_KEY:
        {
            DOT11_BYTE_ARRAY *b = buf;
            if (len >= FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer) + FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey) && b->uNumOfBytes >= FIELD_OFFSET(DOT11_CIPHER_KEY_MAPPING_KEY_VALUE, ucKey)) {
                DOT11_CIPHER_KEY_MAPPING_KEY_VALUE *k = (PVOID)b->ucBuffer;
                if (!k->bDelete && k->AlgorithmId == DOT11_CIPHER_ALGO_CCMP && k->usKeyLength >= FIELD_OFFSET(DOT11_KEY_ALGO_CCMP, ucCCMPKey) + 16 && a->bind.key) {
                    DOT11_KEY_ALGO_CCMP *c = (PVOID)k->ucKey;
                    a->bind.key(0, 0, c->ucCCMPKey, c->ulCCMPKeyLength);
                }
            }
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_CIPHER_DEFAULT_KEY:
        {
            DOT11_CIPHER_DEFAULT_KEY_VALUE *k = buf;
            if (len >= FIELD_OFFSET(DOT11_CIPHER_DEFAULT_KEY_VALUE, ucKey) && !k->bDelete && k->AlgorithmId == DOT11_CIPHER_ALGO_CCMP && k->usKeyLength >= FIELD_OFFSET(DOT11_KEY_ALGO_CCMP, ucCCMPKey) + 16 && a->bind.key) {
                DOT11_KEY_ALGO_CCMP *c = (PVOID)k->ucKey;
                a->bind.key(k->uKeyIndex, 1, c->ucCCMPKey, c->ulCCMPKeyLength);
            }
        }
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_NIC_POWER_STATE:
    case OID_DOT11_HARDWARE_PHY_STATE:
        return NDIS_STATUS_SUCCESS;
    }
    if (oid == OID_802_3_MULTICAST_LIST)
        return NDIS_STATUS_SUCCESS;
    for (i = 0; i < RTL_NUMBER_OF(g_oids); i++)
        if (g_oids[i] == oid)
            return NDIS_STATUS_SUCCESS;         // stage 1: accepted and ignored
    return NDIS_STATUS_NOT_SUPPORTED;
}

static NDIS_STATUS Method(ADAPTER *a, PNDIS_OID_REQUEST r)
{
    switch (r->DATA.METHOD_INFORMATION.Oid) {
    case OID_DOT11_RESET_REQUEST:
        if (a->bind.disconnect)
            a->bind.disconnect();
        InterlockedExchange(&a->assoc, 0);
        if (r->DATA.METHOD_INFORMATION.OutputBufferLength >= sizeof(DOT11_STATUS_INDICATION)) {
            NdisZeroMemory(r->DATA.METHOD_INFORMATION.InformationBuffer, sizeof(DOT11_STATUS_INDICATION));
            r->DATA.METHOD_INFORMATION.BytesWritten = sizeof(DOT11_STATUS_INDICATION);
        }
        r->DATA.METHOD_INFORMATION.BytesRead = r->DATA.METHOD_INFORMATION.InputBufferLength;
        return NDIS_STATUS_SUCCESS;
    case OID_DOT11_ENUM_BSS_LIST:
        {
            DOT11_BYTE_ARRAY *b = r->DATA.METHOD_INFORMATION.InformationBuffer;
            ULONG room = r->DATA.METHOD_INFORMATION.OutputBufferLength, hdr = FIELD_OFFSET(DOT11_BYTE_ARRAY, ucBuffer), head = FIELD_OFFSET(DOT11_BSS_ENTRY, ucBuffer);
            ULONG total = 0, used = 0, i;
            KIRQL k;
            LARGE_INTEGER now;

            KeQuerySystemTime(&now);
            if (room < hdr) {
                r->DATA.METHOD_INFORMATION.BytesNeeded = hdr;
                return NDIS_STATUS_BUFFER_TOO_SHORT;
            }
            KeAcquireSpinLock(&a->lock, &k);
            for (i = 0; i < BSS_MAX; i++) {
                BSS *s = &a->bss[i];
                ULONG need;
                if (!s->used)
                    continue;
                if ((ULONG64)now.QuadPart - s->host > BSS_AGE) {
                    s->used = 0;
                    continue;
                }
                need = head + s->ielen;
                total += need;
                if (hdr + used + need <= room) {
                    DOT11_BSS_ENTRY *e = (PVOID)(b->ucBuffer + used);
                    NdisZeroMemory(e, head);
                    e->uPhyId = PhyOfIes(s->ies, s->ielen, ChanFreq(s->chan));
                    e->PhySpecificInfo.uChCenterFrequency = ChanFreq(s->chan);
                    NdisMoveMemory(e->dot11BSSID, s->bssid, 6);
                    e->dot11BSSType = dot11_BSS_type_infrastructure;
                    e->lRSSI = s->rssi;
                    e->uLinkQuality = s->rssi <= -100 ? 0 : s->rssi >= -50 ? 100 : (ULONG)(2 * (s->rssi + 100));
                    e->bInRegDomain = TRUE;
                    e->usBeaconPeriod = s->bint;
                    e->ullTimestamp = s->ts;
                    e->ullHostTimestamp = s->host;
                    e->usCapabilityInformation = s->cap;
                    e->uBufferLength = s->ielen;
                    NdisMoveMemory(e->ucBuffer, s->ies, s->ielen);
                    used += need;
                }
            }
            KeReleaseSpinLock(&a->lock, k);
            Hdr(&b->Header, DOT11_BSS_ENTRY_BYTE_ARRAY_REVISION_1, sizeof(*b));
            b->uNumOfBytes = used;
            b->uTotalNumOfBytes = total;
            r->DATA.METHOD_INFORMATION.BytesWritten = hdr + used;
            r->DATA.METHOD_INFORMATION.BytesRead = r->DATA.METHOD_INFORMATION.InputBufferLength;
            r->DATA.METHOD_INFORMATION.BytesNeeded = hdr + total;
            return used == total ? NDIS_STATUS_SUCCESS : NDIS_STATUS_BUFFER_OVERFLOW;
        }
    }
    return NDIS_STATUS_NOT_SUPPORTED;
}

static int Armed(const WCHAR *path, ULONG opts);

static NDIS_STATUS OidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST r)
{
    ADAPTER *a = ctx;
    NDIS_STATUS st;

    if (a->try_set && KeGetCurrentIrql() == PASSIVE_LEVEL) {       // survived 2 min: the next boot may start the adapter again
        LARGE_INTEGER now;
        KeQuerySystemTime(&now);
        if (now.QuadPart - a->try_t.QuadPart > 1200000000LL) {
            a->try_set = 0;
            Armed(L"\\??\\C:\\j606f\\drv\\wifi.try", FILE_DELETE_ON_CLOSE);
            Log("crash guard cleared\r\n");
        }
    }

    switch (r->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        st = Query(a, r->DATA.QUERY_INFORMATION.Oid, r->DATA.QUERY_INFORMATION.InformationBuffer, r->DATA.QUERY_INFORMATION.InformationBufferLength,
            (PULONG)&r->DATA.QUERY_INFORMATION.BytesWritten, (PULONG)&r->DATA.QUERY_INFORMATION.BytesNeeded);
        LogOid('q', r->DATA.QUERY_INFORMATION.Oid, r->DATA.QUERY_INFORMATION.InformationBufferLength, st);
        return st;
    case NdisRequestSetInformation:
        st = SetOid(a, r, r->DATA.SET_INFORMATION.Oid, r->DATA.SET_INFORMATION.InformationBuffer, r->DATA.SET_INFORMATION.InformationBufferLength);
        LogOid('s', r->DATA.SET_INFORMATION.Oid, r->DATA.SET_INFORMATION.InformationBufferLength, st);
        return st;
    case NdisRequestMethod:
        st = Method(a, r);
        LogOid('m', r->DATA.METHOD_INFORMATION.Oid, r->DATA.METHOD_INFORMATION.InputBufferLength, st);
        return st;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static void Unload(PDRIVER_OBJECT d) { UNREFERENCED_PARAMETER(d); NdisMDeregisterMiniportDriver(g_drv); if (g_cb) ObDereferenceObject(g_cb); }

static int Armed(const WCHAR *path, ULONG opts)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io;
    HANDLE h;

    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &io, NULL, 0, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | opts, NULL, 0)))
        return 0;
    ZwClose(h);
    return 1;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT d, PUNICODE_STRING reg)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c;

    if (!Armed(L"\\??\\C:\\j606f\\drv\\wifi.on", 0) && !Armed(L"\\??\\C:\\j606f\\drv\\wifi.arm", FILE_DELETE_ON_CLOSE))
        return STATUS_UNSUCCESSFUL;
    if (Armed(L"\\??\\C:\\j606f\\drv\\wifi.try", FILE_DELETE_ON_CLOSE)) {    // the previous start did not survive 2 min: skip one boot, so a crash cannot loop
        Log("--- previous start did not survive, skipping this boot ---\r\n");
        return STATUS_UNSUCCESSFUL;
    }
    {
        UNICODE_STRING us;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK io;
        HANDLE h;
        RtlInitUnicodeString(&us, L"\\??\\C:\\j606f\\drv\\wifi.try");
        InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)))
            ZwClose(h);
    }
    Log("--- j606fwifi driver entry ---\r\n");
    {
        UNICODE_STRING n;
        OBJECT_ATTRIBUTES oa;
        RtlInitUnicodeString(&n, L"\\Callback\\J606fWlan");
        InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        if (!NT_SUCCESS(ExCreateCallback(&g_cb, &oa, TRUE, TRUE)))
            g_cb = NULL;
    }
    NdisZeroMemory(&c, sizeof(c));
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6;
    c.MinorNdisVersion = 30;
    c.MajorDriverVersion = 1;
    c.InitializeHandlerEx = Initialize;
    c.HaltHandlerEx = Halt;
    c.UnloadHandler = Unload;
    c.PauseHandler = Pause;
    c.RestartHandler = Restart;
    c.OidRequestHandler = OidRequest;
    c.SendNetBufferListsHandler = Send;
    c.ReturnNetBufferListsHandler = ReturnNbl;
    c.CancelSendHandler = CancelSend;
    c.ResetHandlerEx = Reset;
    c.DevicePnPEventNotifyHandler = PnpEvent;
    c.ShutdownHandlerEx = Shutdown;
    c.CancelOidRequestHandler = CancelOid;
    return NdisMRegisterMiniportDriver(d, reg, NULL, &c, &g_drv);
}
