#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include <nvml.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

#define REFRESH_DURATION 1
#define BUFFER_SIZE 1024

#define HOTSPOT_REGISTER_OFFSET 0x0002046C
#define VRAM_REGISTER_OFFSET 0x0000E2A8
#define PG_SZ sysconf(_SC_PAGE_SIZE)
#define MEM_PATH "/dev/mem"

#define SEPARATOR     "\xE2\x94\x82"
#define CURSOR_HIDE   "\x1B[?25l"
#define CURSOR_SHOW   "\x1B[?25h"
#define COLOR_RESET   "\x1B[0m"
#define COLOR_GREEN   "\x1B[32m"
#define COLOR_YELLOW  "\x1B[33m"
#define COLOR_RED     "\x1B[31m"

const uint32_t GPU_TEMP_WARN = 70;
const uint32_t GPU_TEMP_DANGER = 85;
const uint32_t JUNCTION_TEMP_WARN = 80;
const uint32_t JUNCTION_TEMP_DANGER = 95;
const uint32_t VRAM_TEMP_WARN = 80;
const uint32_t VRAM_TEMP_DANGER = 95;

static volatile sig_atomic_t running = 1;

typedef struct {
    nvmlReturn_t result;
    unsigned int device_count;
    int initialized;
    struct pci_access *pacc;
    char output_buffer[BUFFER_SIZE];
    size_t buffer_pos;
} Context;

typedef struct {
    nvmlDevice_t device;
    nvmlPciInfo_t pci_info;
    uint32_t gpu_temp;
    uint32_t junction_temp;
    uint32_t vram_temp;
} GpuDevice;

static void cleanup_context(Context *ctx) {
    if (!ctx) return;

    if (ctx->initialized) {
        nvmlShutdown();
        ctx->initialized = 0;
    }

    if (ctx->pacc) {
        pci_cleanup(ctx->pacc);
        ctx->pacc = NULL;
    }
}

static int check_root_privileges(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "This program requires root privileges\n");
        return -1;
    }
    return 0;
}

static void signal_handler(int signum) {
    running = 0;
}

static void restore_cursor(void) {
    printf(CURSOR_SHOW);
    fflush(stdout);
}

static void buffer_append(Context *ctx, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int remaining = BUFFER_SIZE - ctx->buffer_pos;
    int written = vsnprintf(ctx->output_buffer + ctx->buffer_pos, remaining, format, args);
    if (written > 0 && written < remaining) ctx->buffer_pos += written;
    va_end(args);
}

static const char* get_temp_color(uint32_t temp, uint32_t warn, uint32_t danger) {
    if (temp >= danger) return COLOR_RED;
    if (temp >= warn) return COLOR_YELLOW;
    return COLOR_GREEN;
}

static void print_gpu_info(Context *ctx, unsigned int index, GpuDevice *gpu) {
    buffer_append(ctx, "%u %s %s%3u°C%s  %s %s%3u°C%s  %s %s%3u°C%s  %s\n",
        index, SEPARATOR,
        get_temp_color(gpu->gpu_temp, GPU_TEMP_WARN, GPU_TEMP_DANGER),
        gpu->gpu_temp, COLOR_RESET, SEPARATOR,
        get_temp_color(gpu->junction_temp, JUNCTION_TEMP_WARN, JUNCTION_TEMP_DANGER),
        gpu->junction_temp, COLOR_RESET, SEPARATOR,
        get_temp_color(gpu->vram_temp, VRAM_TEMP_WARN, VRAM_TEMP_DANGER),
        gpu->vram_temp, COLOR_RESET, SEPARATOR);
}

static int init_pci(Context *ctx) {
    ctx->pacc = pci_alloc();
    if (!ctx->pacc) {
        fprintf(stderr, "Failed to allocate PCI structure\n");
        return -1;
    }
    pci_init(ctx->pacc);
    pci_scan_bus(ctx->pacc);
    return 0;
}

static int init_nvml(Context *ctx) {
    ctx->result = nvmlInit();
    if (NVML_SUCCESS != ctx->result) {
        fprintf(stderr, "Failed to initialize NVML: %s\n",
          nvmlErrorString(ctx->result));
        return -1;
    }
    ctx->initialized = 1;
    return 0;
}

static int get_device_count(Context *ctx) {
    ctx->result = nvmlDeviceGetCount(&ctx->device_count);
    if (NVML_SUCCESS != ctx->result) {
        fprintf(stderr, "Failed to get device count: %s\n",
          nvmlErrorString(ctx->result));
        return -1;
    }
    if (ctx->device_count == 0) {
        fprintf(stderr, "No NVIDIA GPUs found\n");
        return -1;
    }
    return 0;
}

static int get_device_handle(Context *ctx, unsigned int index, nvmlDevice_t *device) {
    ctx->result = nvmlDeviceGetHandleByIndex(index, device);
    if (NVML_SUCCESS != ctx->result) {
        fprintf(stderr, "Failed to get handle for GPU %u: %s\n",
          index, nvmlErrorString(ctx->result));
        return -1;
    }
    return 0;
}

static int get_device_pci_info(Context *ctx, nvmlDevice_t device, nvmlPciInfo_t *pci_info) {
    ctx->result = nvmlDeviceGetPciInfo(device, pci_info);
    if (NVML_SUCCESS != ctx->result) {
        fprintf(stderr, "Failed to get PCI info: %s\n",
          nvmlErrorString(ctx->result));
        return -1;
    }
    return 0;
}

static int get_gpu_temp(nvmlDevice_t device, uint32_t *temp) {
    nvmlReturn_t result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, temp);
    if (NVML_SUCCESS != result) {
        fprintf(stderr, "Failed to get GPU temperature: %s\n",
          nvmlErrorString(result));
        return -1;
    }
    return 0;
}

static int read_register_temp(struct pci_dev *dev, uint32_t offset, uint32_t *temp) {
    int fd = open(MEM_PATH, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", MEM_PATH, strerror(errno));
        return -1;
    }

    uint32_t reg_addr = (dev->base_addr[0] & 0xFFFFFFFF) + offset;
    uint32_t base_offset = reg_addr & ~(PG_SZ-1);
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
    }

    munmap(map_base, PG_SZ);
    close(fd);

    return (*temp < 0x7f) ? 0 : -1;
}

static int get_gpu_temps(Context *ctx, unsigned int index, GpuDevice *gpu) {
    if ((get_device_handle(ctx, index, &gpu->device) < 0) ||
        (get_gpu_temp(gpu->device, &gpu->gpu_temp) < 0) ||
        (get_device_pci_info(ctx, gpu->device, &gpu->pci_info) < 0))
        return -1;

    for (struct pci_dev *dev = ctx->pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);

        if ((dev->device_id << 16 | dev->vendor_id) != gpu->pci_info.pciDeviceId ||
            (unsigned int)dev->domain != gpu->pci_info.domain ||
            dev->bus != gpu->pci_info.bus ||
            dev->dev != gpu->pci_info.device) {
            continue;
        }

        int junction_result =
          read_register_temp(dev, HOTSPOT_REGISTER_OFFSET, &gpu->junction_temp);
        if (junction_result != 0) return -1;

        int vram_result =
          read_register_temp(dev, VRAM_REGISTER_OFFSET, &gpu->vram_temp);
        if (vram_result != 0) return -1;

        return 0;
    }

    return -1;
}

static int monitor_temperatures(Context *ctx) {
    static int refresh_counter = 0;
    int valid_readings = 0;

    ctx->buffer_pos = 0;
    refresh_counter = refresh_counter == 0 ? 1 : 0;
    buffer_append(ctx, "\n%s", refresh_counter == 0 ? "* " : "  ");
    buffer_append(ctx, "%s  CORE  %s  JUNC  %s  VRAM  %s\n",
      SEPARATOR, SEPARATOR, SEPARATOR, SEPARATOR);

    for (unsigned int i = 0; i < ctx->device_count; i++) {
        GpuDevice gpu = {0};
        if (get_gpu_temps(ctx, i, &gpu) != 0) return -1;
        print_gpu_info(ctx, i, &gpu);
        valid_readings++;
    }

    buffer_append(ctx, "\033[%dA", valid_readings + 2);
    printf("%s", ctx->output_buffer);
    return 0;
}

static int init_monitoring(Context *ctx) {
    if ((check_root_privileges() < 0) ||
        (init_pci(ctx) < 0) ||
        (init_nvml(ctx) < 0) ||
        (get_device_count(ctx) < 0))
        return -1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    printf(CURSOR_HIDE);
    atexit(restore_cursor);
    return 0;
}

int main(void) {
    Context ctx = {0};

    if (init_monitoring(&ctx) < 0) {
        cleanup_context(&ctx);
        return 1;
    }

    while (running) {
        if (monitor_temperatures(&ctx) != 0) break;

        struct timeval tv = {0, (int)(REFRESH_DURATION * 1000000)};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            if (getchar() == '\n') break;
        }
    }

    printf("\033[%dB", ctx.device_count + 2);
    cleanup_context(&ctx);
    return 0;
}
