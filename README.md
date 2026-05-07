# b50-sriov-alloc

Vulkan application for Intel Arc Pro B50 (Battlemage) SR-IOV VF provisioning. By Default it allocates 2GB GPU memory before creating VFs to ensure host has usable VRAM.

## Dependencies (Ubuntu)

```bash
sudo apt install libvulkan-dev vulkan-tools mesa-vulkan-drivers cmake g++ make
```

## Build

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

**Run as root** (required for sysfs SR-IOV writes):

```bash
# Auto-detect Intel Arc Pro B50 and create 3 VFs
sudo ./b50-sriov-alloc --sriov 3

# Specify PCI address manually (format: domain:bus:device, function=0 is forced)
sudo ./b50-sriov-alloc --pci 0000:0d:00 --sriov 3
```

### Examples

```bash
# Auto-detect, create 3 VFs, allocate 2GB
sudo ./b50-sriov-alloc --sriov 3

# Allocate 512 MB, create 3 VFs
sudo ./b50-sriov-alloc --memory 512 --sriov 3

# Allocate 4GB, create 6 VFs
sudo ./b50-sriov-alloc --memory 4096 --sriov 6

# Multi-GPU: specify device explicitly (domain:bus:device)
sudo ./b50-sriov-alloc --pci 0000:0d:00 --sriov 3
```

## Notes

- If multiple Intel Arc Pro B50 devices found use `--pci` to specify
- To force Mesa Vulkan to report only one default GPU (typically VF 0), use:

```bash
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
```
