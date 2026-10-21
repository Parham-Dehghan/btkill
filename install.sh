#!/bin/bash
# btkill installer — Linux/Kali
set -e

echo "[*] btkill installer"

if command -v apt >/dev/null 2>&1; then
    echo "[*] Installing dependencies (apt)..."
    sudo apt update
    sudo apt install -y build-essential libbluetooth-dev bluez
elif command -v pacman >/dev/null 2>&1; then
    echo "[*] Installing dependencies (pacman)..."
    sudo pacman -S --needed base-devel bluez
elif command -v dnf >/dev/null 2>&1; then
    echo "[*] Installing dependencies (dnf)..."
    sudo dnf install -y gcc-c++ bluez-libs-devel bluez
else
    echo "[!] Unsupported distro. Install g++, libbluetooth-dev, bluez manually."
    exit 1
fi

echo "[*] Building linux version..."
cd "$(dirname "$0")/linux"
make

echo ""
echo "[+] Done."
echo "    Scan nearby devices : sudo ./btcpp -s"
echo "    Help                : sudo ./btcpp -h"