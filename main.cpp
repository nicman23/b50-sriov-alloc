#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <vulkan/vulkan.hpp>

struct PciAddress {
    uint16_t domain;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

int setup_sriov_vfs(const PciAddress& pci_addr, int num_vfs) {
    int ret = 1;  // Default to error
    char sysfs_path[128];
    int current_vfs = 0;
    int final_vfs = 0;

    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%d/sriov_numvfs",
             pci_addr.domain, pci_addr.bus, pci_addr.device, pci_addr.function);

    // Check current VFs
    std::ifstream current_file(sysfs_path);
    if (!current_file.is_open()) {
        std::fprintf(stderr, "ERROR: Cannot open %s (device may not support SR-IOV)\n", sysfs_path);
        return 1;
    }

    current_file >> current_vfs;
    current_file.close();

    if (current_vfs == num_vfs) {
        std::printf("Already configured with %d VFs\n", num_vfs);
        return 0;
    }

    // Disable existing VFs first
    if (current_vfs > 0) {
        std::printf("Disabling %d existing VFs...\n", current_vfs);
        std::ofstream disable_file(sysfs_path);
        if (disable_file.is_open()) {
            disable_file << 0;
            disable_file.close();
        }
        sleep(1);
    }

    // Enable VFs (running as root)
    std::printf("Creating %d VFs on %02x:%02x.%d...\n", num_vfs,
                pci_addr.bus, pci_addr.device, pci_addr.function);

    std::ofstream enable_file(sysfs_path);
    if (!enable_file.is_open()) {
        std::fprintf(stderr, "ERROR: Cannot write to %s\n", sysfs_path);
        goto err;
    }

    enable_file << num_vfs;
    enable_file.close();

    if (enable_file.fail()) {
        std::fprintf(stderr, "ERROR: Failed to write to %s\n", sysfs_path);
        goto err;
    }

    // Verify
    sleep(1);
    {
        std::ifstream verify_file(sysfs_path);
        verify_file >> final_vfs;
        verify_file.close();
    }

    if (final_vfs != num_vfs) {
        std::fprintf(stderr, "ERROR: Verification failed (expected %d, got %d)\n",
                     num_vfs, final_vfs);
        goto err;
    }

    std::printf("SUCCESS: Created %d VFs\n", num_vfs);
    ret = 0;

err:
    return ret;
}

void print_usage(const char* program) {
    std::printf("Usage: %s [options]\n", program);
    std::printf("Options:\n");
    std::printf("  --pci <domain:bus:device>  Target PCI device (default: auto-detect)\n");
    std::printf("  --sriov <num>              Enable SR-IOV with specified number of VFs\n");
    std::printf("  --memory <MB>              GPU memory to allocate in MB (default: 2048)\n");
    std::printf("  --help                     Show this help message\n");
}

// Returns: 0 = not found, 1 = found one, 2 = found multiple
int detect_intel_b50_pci(PciAddress& out_addr, std::vector<PciAddress>& found_devices) {
    int ret = 0;  // Default to not found
    const char* sysfs_path = "/sys/bus/pci/devices";
    DIR* dir = nullptr;
    struct dirent* entry;

    dir = opendir(sysfs_path);
    if (!dir) {
        ret = 0;
        goto err;
    }

    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and .. and hidden files
        if (entry->d_name[0] == '.') continue;

        char vendor_path[256], device_path_str[256];
        snprintf(vendor_path, sizeof(vendor_path), "%s/%s/vendor", sysfs_path, entry->d_name);
        snprintf(device_path_str, sizeof(device_path_str), "%s/%s/device", sysfs_path, entry->d_name);

        std::ifstream vendor_file(vendor_path);
        std::ifstream dev_file(device_path_str);

        if (vendor_file.is_open() && dev_file.is_open()) {
            uint16_t vendor_id = 0, device_id = 0;
            vendor_file >> std::hex >> vendor_id;
            dev_file >> std::hex >> device_id;

            // Intel vendor ID: 0x8086
            // Battlemage (BMG) device IDs: 0xe212 (Arc Pro B50), FIXME
            if (vendor_id == 0x8086 && (device_id == 0xe212)) {

                unsigned int domain, bus, dev, func;
                if (std::sscanf(entry->d_name, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) {
                    // Only consider function 0 (main VGA controller)
                    if (func != 0) continue;

                    PciAddress addr{};
                    addr.domain = static_cast<uint16_t>(domain);
                    addr.bus = static_cast<uint8_t>(bus);
                    addr.device = static_cast<uint8_t>(dev);
                    addr.function = static_cast<uint8_t>(func);
                    found_devices.push_back(addr);
                }
            }
        }
    }

    if (found_devices.empty()) {
        ret = 0;
    } else if (found_devices.size() == 1) {
        out_addr = found_devices[0];
        ret = 1;
    } else {
        ret = 2;
    }

err:
    if (dir) closedir(dir);
    return ret;
}

int main(int argc, char* argv[]) {
    uint16_t target_domain = 0;
    uint8_t target_bus = 0;
    uint8_t target_device = 0;
    uint8_t target_function = 0;
    bool auto_detect = true;
    int num_vfs = 0;
    uint32_t memory_mb = 2048;
    
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory device_memory = VK_NULL_HANDLE;
    VkResult result;
    
    VkApplicationInfo app_info{};
    VkInstanceCreateInfo instance_info{};
    VkDeviceQueueCreateInfo queue_info{};
    VkDeviceCreateInfo device_info{};
    VkBufferCreateInfo buffer_info{};
    VkMemoryAllocateInfo alloc_info{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    
    uint32_t memory_type_index = UINT32_MAX;
    VkDeviceSize allocation_size = 0;
    float queue_priority = 1.0f;
    
    std::vector<VkPhysicalDevice> physical_devices;

    // Parse command line arguments

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        
        // Quick first-character check before strcmp
        switch (arg[2]) {
            case 'p':  // --pci
                if (strcmp(arg, "--pci") == 0 && i + 1 < argc) {
                    sscanf(argv[++i], "%hx:%hhx:%hhx", &target_domain, &target_bus, &target_device);
                    auto_detect = false;
                }
                break;
            case 's':  // --sriov
                if (strcmp(arg, "--sriov") == 0 && i + 1 < argc) {
                    num_vfs = atoi(argv[++i]);
                }
                break;
            case 'm':  // --memory
                if (strcmp(arg, "--memory") == 0 && i + 1 < argc) {
                    memory_mb = (uint32_t)atoi(argv[++i]);
                }
                break;
            case 'h':  // --help
                if (strcmp(arg, "--help") == 0) {
                    print_usage(argv[0]);
                    return 0;
                }
                break;
        }
    }

    // Auto-detect Intel Arc Pro B50 if not specified
    PciAddress target_pci{};
    if (auto_detect) {
        std::vector<PciAddress> found_devices;
        int result = detect_intel_b50_pci(target_pci, found_devices);

        if (result == 0) {
            std::fprintf(stderr, "ERROR: Could not auto-detect Intel Arc Pro B50\n");
            std::fprintf(stderr, "Use --pci <bus:device.func> to specify manually\n");
            return 1;
        } else if (result == 2) {
            std::fprintf(stderr, "ERROR: Multiple Intel Arc Pro B50 devices found:\n");
            for (const auto& addr : found_devices) {
                std::fprintf(stderr, "  - %04x:%02x:%02x.%d\n", addr.domain, addr.bus, addr.device, addr.function);
            }
            std::fprintf(stderr, "Use --pci <domain:bus:device> to specify which one to use\n");
            return 1;
        } else {
            target_bus = target_pci.bus;
            target_device = target_pci.device;
            target_function = target_pci.function;
            std::printf("Auto-detected Intel Arc Pro B50 at PCI %04x:%02x:%02x.%d\n",
                        target_pci.domain, target_bus, target_device, target_function);
        }
    } else {
        target_pci.domain = target_domain;
        target_pci.bus = target_bus;
        target_pci.device = target_device;
        target_pci.function = 0;  // Function 0 is the main device
        std::printf("Target PCI device: %04x:%02x:%02x.%d\n", target_domain, target_bus, target_device, 0);
    }

    // Allocate GPU memory on first Intel GPU
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "b50-sriov-alloc";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "NoEngine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create Vulkan instance: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        return 1;
    }

    // Enumerate physical devices
    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        std::fprintf(stderr, "Failed to enumerate physical devices: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_destroy_instance;
    }

    physical_devices.resize(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data());
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to enumerate physical devices: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_destroy_instance;
    }

    // Select Intel Arc Pro B50 (device ID 0xe212)
    std::printf("\nAvailable Vulkan devices:\n");
    for (const auto& dev : physical_devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        std::printf("  - %s (vendor 0x%04x, device 0x%04x)\n", props.deviceName, props.vendorID, props.deviceID);

        if (props.vendorID == 0x8086 && props.deviceID == 0xe212 && physical_device == VK_NULL_HANDLE) {
            physical_device = dev;
            std::printf("    ^-- SELECTED (Arc Pro B50)\n");
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        std::fprintf(stderr, "\nERROR: No Intel Arc Pro B50 (device 0xe212) found\n");
        goto err_destroy_instance;
    }

    std::printf("\nSelected Intel Arc Pro B50 for memory allocation\n");

    // Logical device
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    result = vkCreateDevice(physical_device, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create logical device: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_destroy_instance;
    }

    // Memory type
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memory_type_index = i;
            break;
        }
    }

    if (memory_type_index == UINT32_MAX) {
        std::fprintf(stderr, "No suitable memory type found\n");
        goto err_destroy_device;
    }

    // Buffer
    allocation_size = (VkDeviceSize)memory_mb * 1024 * 1024;

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = allocation_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create buffer: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_destroy_device;
    }

    // Memory allocation
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(device, buffer, &mem_requirements);

    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = memory_type_index;

    result = vkAllocateMemory(device, &alloc_info, nullptr, &device_memory);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to allocate device memory: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_destroy_buffer;
    }

    // Bind memory
    result = vkBindBufferMemory(device, buffer, device_memory, 0);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to bind buffer memory: %s\n", vk::to_string( static_cast<vk::Result>( result ) ).c_str());
        goto err_free_memory;
    }

    std::printf("\nSuccessfully allocated %u MB of GPU memory\n", memory_mb);
    std::printf("PCI device: %04x:%02x:%02x.%d\n", target_pci.domain, target_pci.bus, target_pci.device, target_pci.function);

    // Setup SR-IOV if requested
    if (num_vfs > 0) {
        if (setup_sriov_vfs(target_pci, num_vfs) != 0) {
            goto err_sriov;
        }
    }

    std::printf("Done.\n");
    std::fflush(stdout);
    return 0;

err_sriov:
    vkDestroyBuffer(device, buffer, nullptr);
err_free_memory:
    vkFreeMemory(device, device_memory, nullptr);
err_destroy_buffer:
    vkDestroyDevice(device, nullptr);
err_destroy_device:
    vkDestroyInstance(instance, nullptr);
err_destroy_instance:
    std::printf("Cleanup complete\n");
    return 1;
}
