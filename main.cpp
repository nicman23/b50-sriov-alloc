#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vulkan/vulkan.h>

// Convert VkResult to string for error reporting
const char* vk_result_to_string(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        default: return "UNKNOWN_VK_RESULT";
    }
}

struct PciAddress {
    uint16_t domain;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

int setup_sriov_vfs(const PciAddress& pci_addr, int num_vfs) {
    char sysfs_path[128];
    snprintf(sysfs_path, sizeof(sysfs_path),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%d/sriov_numvfs",
             pci_addr.domain, pci_addr.bus, pci_addr.device, pci_addr.function);

    // Check current VFs
    std::ifstream current_file(sysfs_path);
    if (!current_file.is_open()) {
        std::fprintf(stderr, "ERROR: Cannot open %s (device may not support SR-IOV)\n", sysfs_path);
        return 1;
    }

    int current_vfs = 0;
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
        return 1;
    }

    enable_file << num_vfs;
    enable_file.close();

    if (enable_file.fail()) {
        std::fprintf(stderr, "ERROR: Failed to write to %s\n", sysfs_path);
        return 1;
    }

    // Verify
    sleep(1);
    std::ifstream verify_file(sysfs_path);
    int final_vfs = 0;
    verify_file >> final_vfs;
    verify_file.close();

    if (final_vfs == num_vfs) {
        std::printf("SUCCESS: Created %d VFs\n", num_vfs);
        return 0;
    } else {
        std::fprintf(stderr, "ERROR: Verification failed (expected %d, got %d)\n",
                     num_vfs, final_vfs);
        return 1;
    }
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
    const char* sysfs_path = "/sys/bus/pci/devices";

    // Scan all entries using popen
    FILE* pipe = popen("ls /sys/bus/pci/devices/ 2>/dev/null", "r");
    if (!pipe) return 0;

    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        // Remove newline
        buf[strcspn(buf, "\n")] = 0;

        char vendor_path[256], device_path_str[256];
        snprintf(vendor_path, sizeof(vendor_path), "%s/%s/vendor", sysfs_path, buf);
        snprintf(device_path_str, sizeof(device_path_str), "%s/%s/device", sysfs_path, buf);

        std::ifstream vendor_file(vendor_path);
        std::ifstream dev_file(device_path_str);

        if (vendor_file.is_open() && dev_file.is_open()) {
            uint16_t vendor_id = 0, device_id = 0;
            vendor_file >> std::hex >> vendor_id;
            dev_file >> std::hex >> device_id;

            // Intel vendor ID: 0x8086
            // Battlemage (BMG) device IDs: 0xe212 (Arc Pro B50), etc.
            if (vendor_id == 0x8086 && (device_id == 0xe212 || device_id == 0xe213 ||
                device_id == 0xe214 || device_id == 0xe215 || device_id == 0xe216 ||
                device_id == 0xe217 || device_id == 0xe218 || device_id == 0xe219 ||
                device_id == 0x56a0 || device_id == 0x56a1 || device_id == 0x56a2)) {

                unsigned int domain, bus, dev, func;
                if (std::sscanf(buf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) == 4) {
                    // Only consider function 0 (main VGA controller) to avoid
                    // counting the same physical device multiple times
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

    pclose(pipe);

    if (found_devices.empty()) {
        return 0;  // Not found
    } else if (found_devices.size() == 1) {
        out_addr = found_devices[0];
        return 1;  // Found one
    } else {
        return 2;  // Found multiple
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    uint16_t target_domain = 0;
    uint8_t target_bus = 0;
    uint8_t target_device = 0;
    uint8_t target_function = 0;  // Always 0 (function 0 = main device)
    bool auto_detect = true;
    int num_vfs = 0;
    uint32_t memory_mb = 2048;  // Default 2GB

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pci") == 0 && i + 1 < argc) {
            // Parse format: domain:bus:device (e.g., 0000:0d:00)
            sscanf(argv[++i], "%hx:%hhx:%hhx", &target_domain, &target_bus, &target_device);
            auto_detect = false;
        } else if (strcmp(argv[i], "--sriov") == 0 && i + 1 < argc) {
            num_vfs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            memory_mb = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
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

    // Allocate GPU memory on first Intel GPU (sysfs already verified target PCI device)
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "b50-sriov-alloc";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "NoEngine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create Vulkan instance: %s\n", vk_result_to_string(result));
        return 1;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to enumerate physical devices: %s\n", vk_result_to_string(result));
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    if (device_count == 0) {
        std::fprintf(stderr, "No Vulkan devices found\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::vector<VkPhysicalDevice> physical_devices(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data());
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to enumerate physical devices: %s\n", vk_result_to_string(result));
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // Select Intel Arc Pro B50 (device ID 0xe212)
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::printf("\nAvailable Vulkan devices:\n");

    for (const auto& device : physical_devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        std::printf("  - %s (vendor 0x%04x, device 0x%04x)\n", props.deviceName, props.vendorID, props.deviceID);

        // Select Intel Arc Pro B50 (device ID 0xe212)
        if (props.vendorID == 0x8086 && props.deviceID == 0xe212 && physical_device == VK_NULL_HANDLE) {
            physical_device = device;
            std::printf("    ^-- SELECTED (Arc Pro B50)\n");
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        std::fprintf(stderr, "\nERROR: No Intel Arc Pro B50 (device 0xe212) found\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::printf("\nSelected Intel Arc Pro B50 for memory allocation\n");

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    float queue_priority = 1.0f;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    VkDevice device;
    result = vkCreateDevice(physical_device, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create logical device: %s\n", vk_result_to_string(result));
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    uint32_t memory_type_index = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memory_type_index = i;
            break;
        }
    }

    if (memory_type_index == UINT32_MAX) {
        std::fprintf(stderr, "No suitable memory type found\n");
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    const VkDeviceSize allocation_size = (VkDeviceSize)memory_mb * 1024 * 1024;  // MB to bytes

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = allocation_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create buffer: %s\n", vk_result_to_string(result));
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(device, buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = memory_type_index;

    VkDeviceMemory device_memory;
    result = vkAllocateMemory(device, &alloc_info, nullptr, &device_memory);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to allocate device memory: %s\n", vk_result_to_string(result));
        vkDestroyBuffer(device, buffer, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    result = vkBindBufferMemory(device, buffer, device_memory, 0);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to bind buffer memory: %s\n", vk_result_to_string(result));
        vkFreeMemory(device, device_memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::printf("\nSuccessfully allocated %u MB of GPU memory\n", memory_mb);
    std::printf("PCI device: %04x:%02x:%02x.%d\n", target_pci.domain, target_pci.bus, target_pci.device, target_pci.function);

    // Setup SR-IOV if requested (after memory allocation)
    if (num_vfs > 0) {
        if (setup_sriov_vfs(target_pci, num_vfs) != 0) {
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, device_memory, nullptr);
            vkDestroyDevice(device, nullptr);
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
    }

    std::printf("Done.\n");
    std::fflush(stdout);

    // Cleanup and exit
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, device_memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("Cleanup complete\n");
    return 0;
}
