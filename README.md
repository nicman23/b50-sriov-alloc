# b50-sriov-alloc

Vulkan application for Intel Arc Pro B50 (Battlemage) SR-IOV VF provisioning. Allocates 2GB GPU memory before creating VFs to ensure host has usable VRAM.

## Dependencies (Ubuntu)

```bash
# Vulkan runtime and development libraries
sudo apt install libvulkan-dev vulkan-tools mesa-vulkan-drivers

# Build tools
sudo apt install cmake g++ make

# For SR-IOV setup (optional, if using ssh for root access)
sudo apt install openssh-server
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

### Command Line Options

| Option | Description |
|--------|-------------|
| `--sriov <num>` | Create specified number of VFs |
| `--pci <domain:bus:device>` | Target PCI device (default: auto-detect, function=0 forced) |
| `--memory <MB>` | GPU memory to allocate in MB (default: 2048) |
| `--help` | Show help message |

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

- Requires root access to write to `/sys/bus/pci/devices/*/sriov_numvfs`
- Auto-detection filters to function 0 to avoid counting same device multiple times
- Error if multiple Intel Arc Pro B50 devices found (use `--pci` to specify)
