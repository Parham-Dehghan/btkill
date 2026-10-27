<div align="center"><img src="https://capsule-render.vercel.app/api?type=waving&height=200&color=0:00F5FF,50:7B2CFF,100:FF00A8&text=BTKILL&fontSize=80&fontColor=FFFFFF&fontAlignY=40&animation=fadeIn" width="100%" alt="BTKILL"/>BLUETOOTH MULTI-VECTOR STRESS TESTING TOOL

"Project" (https://img.shields.io/badge/PROJECT-BTKILL-00D9FF?style=for-the-badge)
"Security" (https://img.shields.io/badge/FOCUS-BLUETOOTH_SECURITY-9B5DE5?style=for-the-badge)
"License" (https://img.shields.io/badge/LICENSE-PROPRIETARY-FF4D8D?style=for-the-badge)

TEST · ANALYZE · STRENGTHEN

A Bluetooth resilience testing tool for authorized security assessments.

</div>---

01 / OVERVIEW

BTKILL is a Bluetooth testing project designed to assess connection resilience, service-discovery behavior, and device responses under controlled testing conditions.

The project combines operational statistics, adaptive pacing, and hardware diagnostics to support authorized Bluetooth security assessments.

«[!CAUTION]
AUTHORIZED TESTING ONLY

Use this software only on devices you own or have explicit authorization to test. Unauthorized Bluetooth interference may disrupt communications and violate applicable laws.»

---

02 / FEATURES

Adaptive Pacing

Adjusts the operation rate according to observed outcomes.

Live Statistics

Reports operations per second and success/failure counters.

Hardware Diagnostics

Detects Bluetooth adapter availability and provides diagnostic information.

Multi-Vector Architecture

Organizes supported Bluetooth testing methods in one command-line tool.

---

03 / TESTING VECTORS

Testing Vector| Linux| Windows
L2CAP Churn| Supported| Limited*
HCI Create Connection| Supported| Unavailable*
RFCOMM Sweep| Supported| Supported
SCO Probe| Supported| Unavailable*
Inquiry Testing| Unavailable| Supported
SDP Testing| Unavailable| Supported

Platform support depends on the operating system, Bluetooth adapter, drivers, permissions, and available APIs. Verify these capabilities against the actual implementation.

---

04 / SYSTEM REQUIREMENTS

Linux / Kali Linux

- Compatible Bluetooth adapter.
- Bluetooth drivers and system services.
- C/C++ build tools when compiling from source.
- Appropriate permissions for authorized testing.

Windows

- Compatible Bluetooth adapter.
- Supported Bluetooth drivers and Windows APIs.
- Appropriate build tools when compiling from source.

---

05 / INSTALLATION

Linux / Kali Linux

Step 1 — Clone the repository

git clone https://github.com/Parham-Dehghan/btkill.git
cd btkill

Step 2 — Prepare the installer

chmod +x install.sh

Step 3 — Run the installer

./install.sh

«[!IMPORTANT]
Review "install.sh" before running it. Confirm that the repository URL and installation commands match the current project.»

---

06 / COMMAND LINE

Display the available command-line options:

./btkill --help

The executable name and available arguments may vary depending on the build configuration.

---

07 / TELEMETRY

Metric| Description
"OPS/S"| Operations per second
"SUCCESS"| Successful operations
"FAILURE"| Failed operations
"ADAPTER"| Bluetooth hardware diagnostics
"CONNECTION"| Connection-attempt outcomes

Actual measurements depend on the hardware, drivers, operating system, and testing environment.

---

08 / RESPONSIBLE USE

- Test only devices you own or are authorized to assess.
- Use a controlled testing environment.
- Avoid disrupting third-party Bluetooth communications.
- Follow applicable laws and organizational security policies.
- Stop testing if unintended interference occurs.

---

09 / LICENSE

PROPRIETARY SOFTWARE — ALL RIGHTS RESERVED

Copyright © 2026 Mohammad Parham Dehghan.

Use, modification, redistribution, and commercial exploitation are subject to the terms of the proprietary license and applicable law.

See ""LICENSE"" (LICENSE) for the full terms.

---

<div align="center"><img src="https://capsule-render.vercel.app/api?type=waving&height=110&section=footer&color=0:FF00A8,50:7B2CFF,100:00F5FF" width="100%" alt="BTKILL footer"/>BTKILL

"BLUETOOTH SECURITY" · "RESILIENCE TESTING"

Developed by Mohamma

d Parham Dehghan

</div>
