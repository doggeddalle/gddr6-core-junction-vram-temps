#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include <nvml.h>
#include <errno.h>
#include <string.h>

#define HOTSPOT_REGISTER_OFFSET 0x0002046C
#define VRAM_REGISTER_OFFSET 0x0000E2A8
#define PG_SZ sysconf(_SC_PAGE_SIZE)
#define MEM_PATH "/dev/mem"

typedef struct {
    nvmlReturn_t result;
    int initialized_nvml;
    struct pci_access *pacc;
    int initialized_pci;
} Context;

// Cleanup function for NVML and PCI resources
static void cleanup_context(Context *ctx) {
    if (!ctx) return;

    if (ctx->initialized_nvml) {
        nvmlShutdown();
        ctx->initialized_nvml = 0;
    }

    if (ctx->initialized_pci && ctx->pacc) {
        pci_cleanup(ctx->pacc);
        ctx->pacc = NULL;
        ctx->initialized_pci = 0;
    }
}

// Initialize PCI access
static int init_pci(Context *ctx) {
    ctx->pacc = pci_alloc();
    if (!ctx->pacc) {
        fprintf(stderr, "Failed to allocate PCI structure\n");
        return -1;
    }
    pci_init(ctx->pacc);
    pci_scan_bus(ctx->pacc);
    ctx->initialized_pci = 1;
    return 0;
}

// Initialize NVML
static int init_nvml(Context *ctx) {
    ctx->result = nvmlInit();
    if (NVML_SUCCESS != ctx->result) {
        fprintf(stderr, "Failed to initialize NVML: %s\n", nvmlErrorString(ctx->result));
        return -1;
    }
    ctx->initialized_nvml = 1;
    return 0;
}

// Get device handle by index
static int get_device_handle(unsigned int index, nvmlDevice_t *device) {
    nvmlReturn_t result = nvmlDeviceGetHandleByIndex(index, device);
    if (NVML_SUCCESS != result) {
        fprintf(stderr, "Failed to get handle for GPU %u: %s\n", index, nvmlErrorString(result));
        return -1;
    }
    return 0;
}

// Get PCI info for a device
static int get_device_pci_info(nvmlDevice_t device, nvmlPciInfo_t *pci_info) {
    nvmlReturn_t result = nvmlDeviceGetPciInfo(device, pci_info);
    if (NVML_SUCCESS != result) {
        fprintf(stderr, "Failed to get PCI info: %s\n", nvmlErrorString(result));
        return -1;
    }
    return 0;
}

// Get GPU core temperature using NVML
static int get_gpu_core_temp_nvml(nvmlDevice_t device, uint32_t *temp) {
    nvmlReturn_t result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, temp);
    if (NVML_SUCCESS != result) {
        fprintf(stderr, "Failed to get GPU core temperature: %s\n", nvmlErrorString(result));
        return -1;
    }
    return 0;
}

// Read temperature from PCI register
static int read_register_temp(struct pci_dev *dev, uint32_t offset, uint32_t *temp) {
    int fd = open(MEM_PATH, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", MEM_PATH, strerror(errno));
        return -1;
    }

    uint32_t reg_addr = (dev->base_addr[0] & 0xFFFFFFFF) + offset;
    uint32_t base_offset = reg_addr & ~(PG_SZ - 1);
    void *map_base = mmap(0, PG_SZ, PROT_READ, MAP_SHARED, fd, base_offset);
    if (map_base == MAP_FAILED) {
        fprintf(stderr, "Failed to map memory: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    uint32_t reg_value = *((uint32_t *)((char *)map_base + (reg_addr - base_offset)));

    if (offset == HOTSPOT_REGISTER_OFFSET) {
        *temp = (reg_value >> 8) & 0xff;
    } else if (offset == VRAM_REGISTER_OFFSET) {
        *temp = (reg_value & 0x00000fff) / 0x20;
    } else {
        fprintf(stderr, "Unknown register offset for temperature reading\n");
        munmap(map_base, PG_SZ);
        close(fd);
        return -1;
    }

    munmap(map_base, PG_SZ);
    close(fd);

    return (*temp < 0x7f) ? 0 : -1; // Basic sanity check for temperature
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <temp_type> <output_file_path>\n", argv[0]);
        fprintf(stderr, "  <temp_type>: core, junction, or vram\n");
        return 1;
    }

    const char *temp_type = argv[1];
    const char *output_file_path = argv[2];
    uint32_t temperature = 0;
    Context ctx = {0};
    int ret_code = 0; // Default to success

    // Initialize NVML and PCI
    if (init_nvml(&ctx) < 0 || init_pci(&ctx) < 0) {
        cleanup_context(&ctx);
        return 1;
    }

    nvmlDevice_t device;
    if (get_device_handle(0, &device) < 0) { // Assuming first GPU (index 0)
        cleanup_context(&ctx);
        return 1;
    }

    if (strcmp(temp_type, "core") == 0) {
        if (get_gpu_core_temp_nvml(device, &temperature) < 0) {
            ret_code = 1;
        }
    } else if (strcmp(temp_type, "junction") == 0) {
        nvmlPciInfo_t pci_info;
        if (get_device_pci_info(device, &pci_info) < 0) {
            ret_code = 1;
        } else {
            struct pci_dev *target_dev = NULL;
            for (struct pci_dev *dev = ctx.pacc->devices; dev; dev = dev->next) {
                pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);
                if ((dev->device_id << 16 | dev->vendor_id) == pci_info.pciDeviceId &&
                    (unsigned int)dev->domain == pci_info.domain &&
                    dev->bus == pci_info.bus &&
                    dev->dev == pci_info.device) {
                    target_dev = dev;
                break;
                    }
            }
            if (target_dev && read_register_temp(target_dev, HOTSPOT_REGISTER_OFFSET, &temperature) == 0) {
                // Temperature successfully read
            } else {
                fprintf(stderr, "Failed to get junction temperature or find matching PCI device.\n");
                ret_code = 1;
            }
        }
    } else if (strcmp(temp_type, "vram") == 0) {
        nvmlPciInfo_t pci_info;
        if (get_device_pci_info(device, &pci_info) < 0) {
            ret_code = 1;
        } else {
            struct pci_dev *target_dev = NULL;
            for (struct pci_dev *dev = ctx.pacc->devices; dev; dev = dev->next) {
                pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);
                if ((dev->device_id << 16 | dev->vendor_id) == pci_info.pciDeviceId &&
                    (unsigned int)dev->domain == pci_info.domain &&
                    dev->bus == pci_info.bus &&
                    dev->dev == pci_info.device) {
                    target_dev = dev;
                break;
                    }
            }
            if (target_dev && read_register_temp(target_dev, VRAM_REGISTER_OFFSET, &temperature) == 0) {
                // Temperature successfully read
            } else {
                fprintf(stderr, "Failed to get VRAM temperature or find matching PCI device.\n");
                ret_code = 1;
            }
        }
    } else {
        fprintf(stderr, "Invalid temperature type: %s. Use 'core', 'junction', or 'vram'.\n", temp_type);
        ret_code = 1;
    }

    if (ret_code == 0) {
        FILE *fp = fopen(output_file_path, "w");
        if (fp == NULL) {
            fprintf(stderr, "Failed to open output file %s: %s\n", output_file_path, strerror(errno));
            ret_code = 1;
        } else {
            fprintf(fp, "%u\n", temperature * 1000); // Write in millidegrees
            fclose(fp);
        }
    }

    cleanup_context(&ctx);
    return ret_code;
}
