
<div align="center">

<img src="https://capsule-render.vercel.app/api?type=waving&height=200&color=0:00F5FF,50:7B2CFF,100:FF00A8&text=BTKILL&fontSize=80&fontColor=FFFFFF&fontAlignY=40&animation=fadeIn" width="100%" alt="BTKILL">

# BTKILL

### BLUETOOTH MULTI-VECTOR STRESS TESTING TOOL

![Project](https://img.shields.io/badge/PROJECT-BTKILL-00D9FF?style=for-the-badge)
![Security](https://img.shields.io/badge/FOCUS-BLUETOOTH_SECURITY-9B5DE5?style=for-the-badge)
![License](https://img.shields.io/badge/LICENSE-PROPRIETARY-FF4D8D?style=for-the-badge)

**TEST · ANALYZE · STRENGTHEN**

Bluetooth resilience testing for authorized security assessments.

</div>

---

## 01 / OVERVIEW

**BTKILL** is a multi-vector Bluetooth testing tool designed to assess connection resilience, service-discovery behavior, and device responses under controlled testing conditions.

The project combines operational statistics, adaptive pacing, and hardware diagnostics to support authorized Bluetooth security assessments.

> [!CAUTION]
> **AUTHORIZED TESTING ONLY**
>
> Use this software only on devices you own or have explicit authorization to test. Unauthorized Bluetooth interference may disrupt communications and violate applicable laws.

---

## 02 / ARCHITECTURE

```mermaid
flowchart TD

subgraph group_linux["Linux tool"]
  node_linux_cli["Linux CLI<br/>[btcpp.cpp]"]
  node_linux_adapter["Adapter detection<br/>[btcpp.cpp]"]
  node_linux_scan["Device scan<br/>[btcpp.cpp]"]
  node_linux_vectors["Bluetooth vectors<br/>[btcpp.cpp]"]
  node_linux_rf["RF disruption<br/>[btcpp.cpp]"]
  node_linux_pacing["Adaptive pacing<br/>[btcpp.cpp]"]
  node_linux_stats["Stats and probing<br/>[btcpp.cpp]"]
end

subgraph group_windows["Windows tool"]
  node_windows_cli["Windows CLI<br/>[main.cpp]"]
  node_windows_adapter["Radio detection<br/>[main.cpp]"]
  node_windows_vectors["Bluetooth vectors<br/>[main.cpp]"]
  node_windows_pacing["Adaptive pacing<br/>[main.cpp]"]
  node_windows_stats["Live stats<br/>[main.cpp]"]
end

subgraph group_shared["Runtime capabilities"]
  node_bluetooth(("Bluetooth device"))
  node_rf_controller(("RF controller"))
end

node_user(("Assessment operator"))

node_user -->|"sets options"| node_linux_cli
node_linux_cli -.->|"checks adapter"| node_linux_adapter
node_linux_cli -->|"can scan"| node_linux_scan
node_linux_cli -->|"selects vectors"| node_linux_vectors
node_linux_vectors -->|"stresses links"| node_bluetooth
node_linux_cli -.->|"configures RF mode"| node_linux_rf
node_linux_rf -.->|"controls"| node_rf_controller
node_linux_vectors -.->|"paces attempts"| node_linux_pacing
node_linux_vectors -.->|"records outcomes"| node_linux_stats
node_user -->|"sets options"| node_windows_cli
node_windows_cli -->|"checks radios"| node_windows_adapter
node_windows_cli -->|"starts vectors"| node_windows_vectors
node_windows_vectors -->|"stresses links"| node_bluetooth
node_windows_vectors -->|"adjusts delay"| node_windows_pacing
node_windows_vectors -->|"updates counters"| node_windows_stats

classDef toneBlue fill:#dbeafe,stroke:#2563eb,stroke-width:1.5px,color:#172554
classDef toneAmber fill:#fef3c7,stroke:#d97706,stroke-width:1.5px,color:#78350f
classDef toneMint fill:#dcfce7,stroke:#16a34a,stroke-width:1.5px,color:#14532d
classDef toneIndigo fill:#e0e7ff,stroke:#4f46e5,stroke-width:1.5px,color:#312e81

class node_linux_cli,node_linux_adapter,node_linux_scan,node_linux_vectors,node_linux_rf,node_linux_pacing,node_linux_stats toneBlue
class node_windows_cli,node_windows_adapter,node_windows_vectors,node_windows_pacing,node_windows_stats toneAmber
class node_bluetooth,node_rf_controller toneMint
class node_user toneIndigo
```

---

## 03 / FEATURES

| FEATURE | DESCRIPTION |
|:--|:--|
| **Adaptive Pacing** | Adjusts the operation rate according to observed outcomes. |
| **Live Statistics** | Reports operations per second and success/failure counters. |
| **Hardware Diagnostics** | Detects Bluetooth adapter availability and provides diagnostic information. |
| **Multi-Vector Architecture** | Organizes supported Bluetooth testing methods in one command-line tool. |

---

## 04 / TESTING VECTORS

| TESTING VECTOR | LINUX | WINDOWS |
|:--|:--:|:--:|
| L2CAP Churn | SUPPORTED | LIMITED* |
| HCI Create Connection | SUPPORTED | UNAVAILABLE* |
| RFCOMM Sweep | SUPPORTED | SUPPORTED |
| SCO Probe | SUPPORTED | UNAVAILABLE* |
| Inquiry Testing | UNAVAILABLE | SUPPORTED |
| SDP Testing | UNAVAILABLE | SUPPORTED |

\*Platform support depends on the operating system, Bluetooth adapter, drivers, permissions, and available APIs. Verify these capabilities against the actual implementation.

---

## 05 / SYSTEM REQUIREMENTS

### Linux / Kali Linux

- Compatible Bluetooth adapter
- Bluetooth drivers and system services
- C/C++ build tools when compiling from source
- Appropriate permissions for authorized testing

### Windows

- Compatible Bluetooth adapter
- Supported Bluetooth drivers and Windows APIs
- Appropriate build tools when compiling from source

---

## 06 / INSTALLATION

### Linux / Kali Linux

**Step 01 — Clone the repository**

```bash
git clone https://github.com/Parham-Dehghan/btkill.git
cd btkill
```

**Step 02 — Prepare the installer**

```bash
chmod +x install.sh
```

**Step 03 — Run the installer**

```bash
./install.sh
```

> [!IMPORTANT]
> Review `install.sh` before running it. Confirm that the repository URL and installation commands match the current project.

---

## 07 / COMMAND LINE

Display the available command-line options:

```bash
./btkill --help
```

The executable name and available arguments may vary depending on the build configuration.

---

## 08 / TELEMETRY

| METRIC | DESCRIPTION |
|:--|:--|
| OPS/S | Operations per second |
| SUCCESS | Successful operations |
| FAILURE | Failed operations |
| ADAPTER | Bluetooth hardware diagnostics |
| CONNECTION | Connection-attempt outcomes |

Actual measurements depend on the hardware, drivers, operating system, and testing environment.

---

## 09 / RESPONSIBLE USE

- Test only devices you own or are authorized to assess.
- Use a controlled testing environment.
- Avoid disrupting third-party Bluetooth communications.
- Follow applicable laws and organizational security policies.
- Stop testing if unintended interference occurs.

---

## 10 / LICENSE

**PROPRIETARY SOFTWARE — ALL RIGHTS RESERVED**

Copyright (c) 2026 Mohammad Parham Dehghan.

Use, modification, redistribution, and commercial exploitation are subject to the terms of the proprietary license and applicable law.

See `LICENSE` for the full terms.

---

<div align="center">

<img src="https://capsule-render.vercel.app/api?type=waving&height=120&section=footer&color=0:FF00A8,50:7B2CFF,100:00F5FF" width="100%" alt="BTKILL footer">

**BTKILL**

BLUETOOTH SECURITY · RESILIENCE TESTING

Developed by Mohammad Parham Dehghan

</div>
