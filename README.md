# Bluetooth-multi-vector-stress-tool
# btkill

Multi-vector Bluetooth link stress-testing tool for **authorized security
assessments only**. Tests the resilience of your own Bluetooth devices
against connection flooding, service-discovery abuse, and inquiry storms.

## Vectors

| Vector | Linux | Windows | Description |
|---|---|---|---|
| L2CAP churn | ✅ | ✅* | Rapid L2CAP connect/send cycles |
| HCI create_conn | ✅ | ❌* | Raw paging storm at the controller |
| RFCOMM sweep | ✅ | ✅ | Channel 1–30 scan + socket hold |
| SCO probe | ✅ | ❌* | Voice-link churn |
| Inquiry storm | ❌ | ✅ | Radio-level discovery hammering |
| SDP flood | ❌ | ✅ | Service-lookup hammering |

\* On Windows, raw HCI/L2CAP/SCO are locked by the OS stack; only what
Winsock exposes is available. On Linux all four vectors are fully armed.

## Features
- Adaptive pacing: speeds up on success, backs off on saturation (low CPU)
- Live stats (`ops/s`, success/fail counters)
- Hardware detection with clear diagnosis when no adapter is found
- Kali-style CLI (Linux): scan, vector mask, duration, threads

## Install

### Linux / Kali
```bash
git clone https://github.com/<YOUR_USER>/btkill.git
cd btkill
chmod +x install.sh && ./install.sh
