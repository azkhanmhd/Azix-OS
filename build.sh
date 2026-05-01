#!/bin/bash
# ============================================================
# Azix OS Build Script
# Run inside WSL2 (Ubuntu) or any Linux environment.
# ============================================================

set -e

echo "============================================"
echo "          Azix OS Build Script"
echo "============================================"
echo ""

# Helper — check a command is available
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "  [MISSING] $1"
        echo "  Install with: $2"
        echo ""
        MISSING=1
    else
        echo "  [OK]      $1"
    fi
}

MISSING=0
echo "Checking dependencies..."
check_cmd nasm               "sudo apt install nasm"
check_cmd i686-linux-gnu-gcc "sudo apt install gcc-i686-linux-gnu binutils-i686-linux-gnu"
check_cmd grub-mkrescue      "sudo apt install grub-pc-bin grub-common xorriso mtools"
check_cmd xorriso            "sudo apt install xorriso"

if [ "$MISSING" -ne 0 ]; then
    echo ""
    echo "One or more tools are missing. Install everything at once with:"
    echo ""
    echo "  sudo apt update && sudo apt install -y \\"
    echo "    nasm gcc-i686-linux-gnu binutils-i686-linux-gnu \\"
    echo "    grub-pc-bin grub-common xorriso mtools"
    echo ""
    exit 1
fi

echo ""
echo "Building..."
echo ""

make clean
make all

echo ""
echo "============================================"
echo "  SUCCESS: azix.iso created!"
echo "============================================"
echo ""
echo "To run in VirtualBox:"
echo "  1. Click 'New'"
echo "  2. Name: Azix OS   Type: Other   Version: Other/Unknown (32-bit)"
echo "  3. Memory: 32 MB (minimum)"
echo "  4. Hard disk: 'Do not add a virtual hard disk'"
echo "  5. Settings -> Storage -> Add Optical Drive -> azix.iso"
echo "  6. Start the VM and enjoy!"
echo ""
