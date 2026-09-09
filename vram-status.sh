#!/bin/bash
# Shows LMEM (VRAM) allocation in GB for the PF and each SR-IOV VF of an
# Intel Arc Pro B50/B70, using the xe driver's SR-IOV debugfs interface.
# Requires VFs to already be created (see run.sh --sriov) and debugfs
# to be mounted. Run as root.

set -e

PCI=$(lspci -nn | grep -E "8086:(e211|e212|e223)" | head -1 | cut -d' ' -f1)
if [ -z "$PCI" ]; then
    echo "ERROR: No Intel Arc Pro B50/B70 found" >&2
    exit 1
fi

PCI_BDF="0000:${PCI}"
SRIOV_DIR="/sys/kernel/debug/dri/${PCI_BDF}/sriov"
RESOURCE_FILE="/sys/bus/pci/devices/${PCI_BDF}/resource"

to_gb() {
    awk -v b="$1" 'BEGIN { printf "%.2f GB", b / 1073741824 }'
}

# BAR2 = LMEM aperture (total tile VRAM)
read -r lmem_start lmem_end _ < <(sed -n '3p' "$RESOURCE_FILE")
total=$(( lmem_end - lmem_start + 1 ))

vf_total=0
echo "VFs:"
for vf in "$SRIOV_DIR"/vf*/; do
    n=$(basename "$vf")
    quota=$(cat "$vf/tile0/vram_quota")
    if [ "$quota" -ne 0 ]; then
        vf_total=$((vf_total + quota))
        echo "  $n: $(to_gb "$quota")"
    fi
done

spare=$(cat "$SRIOV_DIR/pf/tile0/gt0/lmem_spare")
pf=$((total - vf_total - spare))

echo "PF:    $(to_gb "$pf")"
echo "Spare: $(to_gb "$spare")"
echo "Total: $(to_gb "$total")"
