<div align="center"><img src="https://capsule-render.vercel.app/api?type=waving&height=220&color=0:00F5FF,50:7B2CFF,100:FF00A8&text=BTKILL&fontSize=80&fontColor=FFFFFF&fontAlignY=38&desc=BLUETOOTH%20SECURITY%20TESTING&descSize=17&descAlignY=58&animation=fadeIn" width="100%" alt="BTKILL animated neon header"/><br/><img src="https://img.shields.io/badge/PROJECT-BTKILL-00F5FF?style=for-the-badge" alt="Project"/>
<img src="https://img.shields.io/badge/SECURITY-BLUETOOTH-9D4EDD?style=for-the-badge" alt="Bluetooth security"/>
<img src="https://img.shields.io/badge/LICENSE-PROPRIETARY-FF3CAC?style=for-the-badge" alt="Proprietary license"/><br/><br/>

<span style="color:#00F5FF">TEST</span> · <span style="color:#9D4EDD">ANALYZE</span> · <span style="color:#FF3CAC">STRENGTHEN</span>

A multi-vector Bluetooth resilience testing tool for authorized security assessments.

<br/>"OVERVIEW" (#overview) · "FEATURES" (#features) · "VECTORS" (#testing-vectors) · "INSTALLATION" (#installation) · "LICENSE" (#license)

</div>---

<a id="overview"></a>

<span style="color:#00F5FF">01 / OVERVIEW</span>

BTKILL is a Bluetooth testing project designed to assess connection resilience, service-discovery behavior, and device responses under controlled testing conditions.

The project combines operational statistics, adaptive pacing, and hardware diagnostics to support authorized Bluetooth security assessments.

«[!CAUTION]
AUTHORIZED TESTING ONLY

Use this software only on devices you own or have explicit authorization to test. Unauthorized Bluetooth interference may disrupt communications and violate applicable laws.»

---

<a id="features"></a>

<span style="color:#9D4EDD">02 / FEATURES</span>

<table>
<tr>
<td width="50%"><h3>Adaptive Pacing</h3>Adjusts the operation rate according to observed outcomes.

</td>
<td width="50%"><h3>Live Statistics</h3>Reports operations per second and success/failure counters.

</td>
</tr>
<tr>
<td width="50%"><h3>Hardware Diagnostics</h3>Detects Bluetooth adapter availability and reports diagnostic information.

</td>
<td width="50%"><h3>Multi-Vector Architecture</h3>Organizes multiple Bluetooth testing methods in one command-line tool.

</td>
</tr>
</table>---

<a id="testing-vectors"></a>

<span style="color:#FF3CAC">03 / TESTING VECTORS</span>

VECTOR| LINUX| WINDOWS
L2CAP Churn| <span style="color:#00D68F">SUPPORTED</span>| <span style="color:#FFC857">LIMITED*</span>
HCI Create Connection| <span style="color:#00D68F">SUPPORTED</span>| <span style="color:#FF5577">UNAVAILABLE*</span>
RFCOMM Sweep| <span style="color:#00D68F">SUPPORTED</span>| <span style="color:#00D68F">SUPPORTED</span>
SCO Probe| <span style="color:#00D68F">SUPPORTED</span>| <span style="color:#FF5577">UNAVAILABLE*</span>
Inquiry Testing| <span style="color:#FF5577">UNAVAILABLE</span>| <span style="color:#00D68F">SUPPORTED</span>
SDP Testing| <span style="color:#FF5577">UNAVAILABLE</span>| <span style="color:#00D68F">SUPPORTED</span>

<sub>* Platform capabilities depend on the operating system, Bluetooth adapter, drivers, permissions, and APIs. Verify support against the actual implementation.</sub>

---

<span style="color:#00F5FF">04 / SYSTEM REQUIREMENTS</span>

<details>
<summary><strong>Linux / Kali Linux</strong></summary>- Compatible Bluetooth adapter
- Configured Bluetooth drivers and system services
- C/C++ build tools when compiling from source
- Appropriate permissions for authorized testing

</details><details>
<summary><strong>Windows</strong></summary>- Compatible Bluetooth adapter
- Supported Bluetooth drivers and Windows APIs
- Appropriate build tools when compiling from source

</details>---

<a id="installation"></a>

<span style="color:#9D4EDD">05 / INSTALLATION</span>

01 — Clone the repository

git clone https://github.com/Parham-Dehghan/btkill.git
cd btkill

02 — Prepare the installer

chmod +x install.sh

03 — Install

./install.sh

«[!IMPORTANT]
Review the installation script before running it. Confirm that the repository URL and commands match the current project.»

---

<span style="color:#FF3CAC">06 / COMMAND LINE</span>

Display the available options:

./btkill --help

Available commands and arguments depend on the current build and source-code implementation.

---

<span style="color:#00F5FF">07 / TELEMETRY</span>

METRIC| DESCRIPTION
"OPS/S"| Operations per second
"SUCCESS"| Successful operations
"FAILURE"| Failed operations
"ADAPTER"| Bluetooth hardware diagnostics
"CONNECTION"| Connection-attempt outcomes

Actual measurements depend on the hardware, drivers, operating system, and testing environment.

---

<span style="color:#9D4EDD">08 / RESPONSIBLE USE</span>

- Test only devices you own or are authorized to assess.
- Use a controlled testing environment.
- Avoid disrupting third-party communications.
- Follow applicable laws and organizational policies.
- Stop testing if unintended interference occurs.

---

<a id="license"></a>

<span style="color:#FF3CAC">09 / LICENSE</span>

PROPRIETARY SOFTWARE — ALL RIGHTS RESERVED

Copyright © 2026 Mohammad Parham Dehghan.

Use, modification, redistribution, and commercial exploitation are subject to the terms of the proprietary license and applicable law.

See ""LICENSE"" (LICENSE) for the full terms.

---

<div align="center"><img src="https://capsule-render.vercel.app/api?type=waving&height=120&color=0:FF00A8,50:7B2CFF,100:00F5FF&section=footer" width="100%" alt="Neon gradient footer"/><span style="color:#00F5FF">BTKILL</span>

<span style="color:#9D4EDD">BLUETOOTH SECURITY</span> / <span style="color:#FF3CAC">RESILIENCE TESTING</span>

Developed by Mohammad Parham Dehghan

<sub>Built for authorized testing and responsible security research.</sub>

</div>
