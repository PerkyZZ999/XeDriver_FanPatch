// SPDX-License-Identifier: GPL-2.0
/*
 * xe_pcode_probe - Userspace PCODE Mailbox Probe Tool for Intel Arc B580
 *
 * Probes undocumented FAN_SPEED_CONTROL subcommands via direct PCI BAR0 MMIO.
 * Reads fan tachometer, temperature, and late-binding firmware status.
 *
 * Usage:
 *   sudo ./xe_pcode_probe --probe          # Probe all FSC subcommands
 *   sudo ./xe_pcode_probe --read-fan       # Read fan RPM
 *   sudo ./xe_pcode_probe --read-temp      # Read package temperature
 *   sudo ./xe_pcode_probe --late-bind      # Query late binding status
 *   sudo ./xe_pcode_probe --write-duty 128 # Attempt to set fan duty (0-255)
 *   sudo ./xe_pcode_probe --interactive    # Interactive mailbox command sender
 *
 * Build: gcc -O2 -Wall -o xe_pcode_probe xe_pcode_probe.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <getopt.h>
#include <dirent.h>

#include "pcode_protocol.h"

#define BAR0_SIZE       (16 * 1024 * 1024)  /* 16MB */

#define B580_VENDOR_ID  0x8086
#define B580_DEVICE_ID  0xe20b

static char pci_bdf[32] = "";
static char bar0_path[256] = "";
static volatile uint32_t *mmio_base = NULL;
static int mem_fd = -1;
static int verbose = 0;

/* Auto-detect PCI BDF for Intel Arc B580 (8086:e20b) */
static int find_b580_pci(char *out, size_t outlen)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *ent;

    if (!dir)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        char device_path[512];
        char vendor_path[512];
        char device_id[16] = "";
        char vendor_id[16] = "";
        FILE *fp;

        if (ent->d_name[0] == '.')
            continue;

        snprintf(device_path, sizeof(device_path),
                 "/sys/bus/pci/devices/%s/device", ent->d_name);
        snprintf(vendor_path, sizeof(vendor_path),
                 "/sys/bus/pci/devices/%s/vendor", ent->d_name);

        fp = fopen(device_path, "r");
        if (!fp)
            continue;
        if (!fgets(device_id, sizeof(device_id), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        fp = fopen(vendor_path, "r");
        if (fp) {
            fgets(vendor_id, sizeof(vendor_id), fp);
            fclose(fp);
        }

        if (strtol(device_id, NULL, 16) == B580_DEVICE_ID &&
            (!vendor_id[0] || strtol(vendor_id, NULL, 16) == B580_VENDOR_ID)) {
            snprintf(out, outlen, "%s", ent->d_name);
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return -1;
}

static int resolve_pci_bdf(void)
{
    const char *env = getenv("XE_PCI_BDF");

    if (pci_bdf[0])
        return 0;

    if (env && env[0]) {
        snprintf(pci_bdf, sizeof(pci_bdf), "%s", env);
        return 0;
    }

    if (find_b580_pci(pci_bdf, sizeof(pci_bdf)) == 0)
        return 0;

    fprintf(stderr, "Error: Intel Arc B580 (8086:e20b) not found on PCI bus\n");
    fprintf(stderr, "Specify BDF with --pci 0000:BB:DD.F or XE_PCI_BDF env var\n");
    return -1;
}

static int read_xe_hwmon_fan1(unsigned long *rpm_out)
{
    DIR *dir = opendir("/sys/class/hwmon");
    struct dirent *ent;
    char path[512];

    if (!dir || !rpm_out)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        FILE *fp;

        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
        fp = fopen(path, "r");
        if (!fp)
            continue;

        char name[32] = "";
        if (!fgets(name, sizeof(name), fp) || strncmp(name, "xe", 2) != 0) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/fan1_input", ent->d_name);
        fp = fopen(path, "r");
        if (!fp)
            continue;

        if (fscanf(fp, "%lu", rpm_out) == 1) {
            fclose(fp);
            closedir(dir);
            return 0;
        }
        fclose(fp);
    }

    closedir(dir);
    return -1;
}

/* Error code names */
static const char *pcode_err_str(uint32_t err)
{
    switch (err) {
    case PCODE_SUCCESS:              return "Success";
    case PCODE_ILLEGAL_CMD:          return "Illegal Command";
    case PCODE_TIMEOUT:              return "Timed out";
    case PCODE_ILLEGAL_DATA:         return "Illegal Data";
    case PCODE_ILLEGAL_SUBCOMMAND:   return "Illegal Subcommand";
    case PCODE_LOCKED:               return "PCODE Locked";
    case PCODE_GT_RATIO_OUT_OF_RANGE: return "GT ratio out of range";
    case PCODE_REJECTED:             return "PCODE Rejected";
    default:                         return "Unknown";
    }
}

/* Map PCI BAR0 into userspace */
static int map_bar0(void)
{
    if (resolve_pci_bdf() < 0)
        return -1;

    snprintf(bar0_path, sizeof(bar0_path),
             "/sys/bus/pci/devices/%s/resource0", pci_bdf);

    mem_fd = open(bar0_path, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", bar0_path, strerror(errno));
        fprintf(stderr, "Try: sudo setpci -s %s COMMAND latency=0\n", pci_bdf);
        return -1;
    }

    void *map = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }

    mmio_base = (volatile uint32_t *)map;
    printf("[INFO] BAR0 mapped at %p (size %dMB)\n", map, BAR0_SIZE / (1024*1024));
    return 0;
}

static void unmap_bar0(void)
{
    if (mmio_base) {
        munmap((void *)mmio_base, BAR0_SIZE);
        mmio_base = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

/* Read a 32-bit MMIO register */
static inline uint32_t mmio_read32(uint32_t offset)
{
    return mmio_base[offset / 4];
}

/* Write a 32-bit MMIO register */
static inline void mmio_write32(uint32_t offset, uint32_t val)
{
    mmio_base[offset / 4] = val;
}

/* Wait for PCODE_READY to clear, with timeout */
static int wait_pcode_ready(uint32_t timeout_us)
{
    uint32_t elapsed = 0;
    uint32_t step = 10; /* 10us steps */

    while (elapsed < timeout_us) {
        if ((mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) == 0)
            return 0;
        usleep(step);
        elapsed += step;
    }
    return -ETIMEDOUT;
}

/*
 * Send a PCODE mailbox read command.
 * Returns 0 on success, negative errno on failure.
 * Response data is in *data0 (and *data1 if non-NULL).
 */
static int pcode_read(uint32_t cmd, uint32_t param1, uint32_t param2,
                      uint32_t *data0, uint32_t *data1)
{
    uint32_t mbox;
    int ret;

    /* Check if mailbox is busy */
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        /* Wait for it to clear */
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) {
            fprintf(stderr, "[ERR] PCODE mailbox busy (READY bit stuck)\n");
            return -EBUSY;
        }
    }

    /* Clear data registers */
    mmio_write32(PCODE_DATA0_OFFSET, 0);
    mmio_write32(PCODE_DATA1_OFFSET, 0);

    /* Send command */
    mbox = PCODE_READY | PCODE_MBOX(cmd, param1, param2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);

    /* Wait for completion */
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) {
        fprintf(stderr, "[ERR] PCODE read timeout (cmd=0x%02x p1=0x%02x p2=0x%02x)\n",
                cmd, param1, param2);
        return ret;
    }

    /* Read response */
    *data0 = mmio_read32(PCODE_DATA0_OFFSET);
    if (data1)
        *data1 = mmio_read32(PCODE_DATA1_OFFSET);

    /* Check for errors */
    uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS) {
        if (verbose || status != PCODE_ILLEGAL_SUBCOMMAND)
            fprintf(stderr, "[ERR] PCODE cmd=0x%02x p1=0x%02x p2=0x%02x: %s (0x%02x)\n",
                    cmd, param1, param2, pcode_err_str(status), status);
        return -EIO;
    }

    return 0;
}

/*
 * Send a PCODE mailbox write command.
 * Returns 0 on success, negative errno on failure.
 */
static int pcode_write(uint32_t cmd, uint32_t param1, uint32_t param2,
                       uint32_t data0, uint32_t data1)
{
    uint32_t mbox;
    int ret;

    /* Check if mailbox is busy */
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) {
            fprintf(stderr, "[ERR] PCODE mailbox busy\n");
            return -EBUSY;
        }
    }

    /* Write data */
    mmio_write32(PCODE_DATA0_OFFSET, data0);
    mmio_write32(PCODE_DATA1_OFFSET, data1);

    /* Send command */
    mbox = PCODE_READY | PCODE_MBOX(cmd, param1, param2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);

    /* Wait for completion */
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) {
        fprintf(stderr, "[ERR] PCODE write timeout (cmd=0x%02x p1=0x%02x p2=0x%02x)\n",
                cmd, param1, param2);
        return ret;
    }

    /* Check for errors */
    uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS) {
        fprintf(stderr, "[ERR] PCODE write cmd=0x%02x p1=0x%02x p2=0x%02x: %s (0x%02x)\n",
                cmd, param1, param2, pcode_err_str(status), status);
        return -EIO;
    }

    return 0;
}

/* Print a timestamp */
static void print_ts(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("[%ld.%06ld] ", ts.tv_sec, ts.tv_nsec / 1000);
}

/* === Command implementations === */

static int cmd_probe(void)
{
    uint32_t data0, data1;
    int ret;
    int found = 0;

    printf("\n=== FAN_SPEED_CONTROL (0x7D) Subcommand Probe ===\n\n");

    /* First, read the number of fans (known working subcommand) */
    ret = pcode_read(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0, &data0, NULL);
    if (ret == 0) {
        printf("  [0x4] FSC_READ_NUM_FANS:  SUCCESS - fan_count=%u\n", data0);
        found++;
    } else {
        printf("  [0x4] FSC_READ_NUM_FANS:  FAILED (ret=%d)\n", ret);
    }

    /* Probe undocumented subcommands */
    printf("\n  Probing undocumented subcommands (0x0-0x3, 0x5-0xF)...\n\n");

    for (uint32_t sub = 0; sub <= FSC_PROBE_MAX; sub++) {
        if (sub == FSC_READ_NUM_FANS)
            continue;

        print_ts();

        /* Try read with no params */
        ret = pcode_read(FAN_SPEED_CONTROL, sub, 0, &data0, &data1);

        if (ret == 0) {
            printf("  [0x%x] READ  SUCCESS - data0=0x%08x data1=0x%08x\n",
                   sub, data0, data1);

            /* Try with different param2 values (fan channel 0,1,2) */
            for (uint32_t ch = 0; ch < BMG_FAN_MAX; ch++) {
                uint32_t d0, d1;
                ret = pcode_read(FAN_SPEED_CONTROL, sub, ch, &d0, &d1);
                if (ret == 0) {
                    printf("         param2=%u (fan%u): data0=0x%08x data1=0x%08x\n",
                           ch, ch + 1, d0, d1);
                }
            }
            found++;
        } else {
            uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
            printf("  [0x%x] READ  FAILED - %s (0x%02x)\n",
                   sub, pcode_err_str(status), status);
        }
    }

    printf("\n=== Probe Complete: %d subcommands returned success ===\n\n", found);

    /* Also probe LATE_BINDING status */
    printf("=== Late Binding Fan Control Status ===\n\n");

    ret = pcode_read(PCODE_LATE_BINDING, GET_CAPABILITY_STATUS, 0, &data0, NULL);
    if (ret == 0) {
        printf("  Capability Status: 0x%08x\n", data0);
        printf("    V1_FAN_SUPPORTED:    %s\n", (data0 & V1_FAN_SUPPORTED) ? "YES" : "NO");
        printf("    VR_PARAMS_SUPPORTED: %s\n", (data0 & VR_PARAMS_SUPPORTED) ? "YES" : "NO");
        printf("    V1_FAN_PROVISIONED:  %s\n", (data0 & V1_FAN_PROVISIONED) ? "YES" : "NO");
        printf("    VR_PARAMS_PROVISIONED: %s\n", (data0 & VR_PARAMS_PROVISIONED) ? "YES" : "NO");
    } else {
        printf("  Capability Status: READ FAILED\n");
    }

    ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_LOW, FAN_TABLE, &data0, NULL);
    if (ret == 0) {
        uint32_t major = (data0 >> 16) & 0xFFFF;
        uint32_t minor = data0 & 0xFFFF;
        printf("  Fan FW Version: %u.%u", major, minor);
    }

    ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_HIGH, FAN_TABLE, &data0, NULL);
    if (ret == 0) {
        uint32_t hotfix = (data0 >> 16) & 0xFFFF;
        uint32_t build = data0 & 0xFFFF;
        printf(".%u.%u\n", hotfix, build);
    } else {
        printf("\n");
    }

    printf("\n");
    return 0;
}

static int cmd_read_fan(void)
{
    uint32_t fan_count = 0;
    uint32_t data0;
    int ret;

    /* Get fan count from PCODE */
    ret = pcode_read(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0, &data0, NULL);
    if (ret == 0) {
        fan_count = data0;
        printf("Fan count (from PCODE): %u\n", fan_count);
    } else {
        printf("Fan count (from PCODE): READ FAILED, assuming 1\n");
        fan_count = 1;
    }

    /* Read tachometer registers */
    uint32_t offsets[3] = {
        BMG_FAN_1_SPEED_OFFSET,
        BMG_FAN_2_SPEED_OFFSET,
        BMG_FAN_3_SPEED_OFFSET
    };

    /* Read initial values */
    uint32_t prev[3] = {0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < BMG_FAN_MAX && i < (int)fan_count; i++) {
        prev[i] = mmio_read32(offsets[i]);
    }

    /* Wait 1 second for pulse accumulation */
    usleep(1000000);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    printf("\nFan Speed Readings (sampled over %.1fms):\n", elapsed_ms);
    printf("-------------------------------------------\n");

    for (int i = 0; i < BMG_FAN_MAX && i < (int)fan_count; i++) {
        uint32_t curr = mmio_read32(offsets[i]);
        uint32_t pulses = curr - prev[i];
        uint32_t rotations = pulses / 2; /* 2 pulses per rotation */
        double rpm = (rotations * 60000.0) / elapsed_ms;

        printf("  fan%d: tach_raw=%u (prev=%u, pulses=%u, rotations=%u) -> %.0f RPM\n",
               i + 1, curr, prev[i], pulses, rotations, rpm);

        /* Also compare with hwmon sysfs */
        if (i == 0) {
            unsigned long hwmon_rpm = 0;
            if (read_xe_hwmon_fan1(&hwmon_rpm) == 0)
                printf("         hwmon fan1_input: %lu RPM\n", hwmon_rpm);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_read_temp(void)
{
    uint32_t pkg_temp_raw = mmio_read32(BMG_PKG_TEMP_OFFSET);
    uint32_t pkg_temp = pkg_temp_raw & 0xFF; /* bits 7:0, degrees Celsius */

    uint32_t vram_temp_raw = mmio_read32(BMG_VRAM_TEMP_OFFSET);
    /* VRAM temp: 24-bit [31:8] signed, 8-bit [7:0] fraction */
    int32_t vram_temp_int = (vram_temp_raw >> 8) & 0xFFFFFF;
    if (vram_temp_int & 0x800000) vram_temp_int -= 0x1000000; /* sign extend */
    uint32_t vram_temp_frac = vram_temp_raw & 0xFF;

    printf("\nTemperature Readings:\n");
    printf("----------------------\n");
    printf("  Package:  %u°C (raw=0x%08x)\n", pkg_temp, pkg_temp_raw);
    printf("  VRAM:     %d.%u°C (raw=0x%08x)\n", vram_temp_int, vram_temp_frac, vram_temp_raw);

    /* Read thermal limits from PCODE */
    uint32_t limits0, limits1;
    int ret = pcode_read(PCODE_THERMAL_INFO, READ_THERMAL_LIMITS, 0, &limits0, &limits1);
    if (ret == 0) {
        printf("\n  Thermal Limits (from PCODE):\n");
        printf("    limits0=0x%08x limits1=0x%08x\n", limits0, limits1);
        /* The xe driver parses these as u8 array */
        uint8_t *l = (uint8_t *)&limits0;
        printf("    [0]=%u [1]=%u [2]=%u [3]=%u (pkg_shutdown, pkg_crit, mem_shutdown, pkg_max)\n",
               l[0], l[1], l[2], l[3]);
        l = (uint8_t *)&limits1;
        printf("    [4]=%u (mem_crit)\n", l[0]);
    }

    /* Read thermal config (sensor count) */
    uint32_t config;
    ret = pcode_read(PCODE_THERMAL_INFO, READ_THERMAL_CONFIG, 0, &config, NULL);
    if (ret == 0) {
        printf("    sensor_count=%u\n", config & 0xFF);
    }
    printf("\n");
    return 0;
}

static int cmd_late_bind(void)
{
    uint32_t data0, data1;
    int ret;

    printf("\n=== Late Binding Firmware Status ===\n\n");

    /* Capability status */
    ret = pcode_read(PCODE_LATE_BINDING, GET_CAPABILITY_STATUS, 0, &data0, NULL);
    if (ret == 0) {
        printf("  Capability Status: 0x%08x\n", data0);
        printf("    V1_FAN_SUPPORTED:      %s\n", (data0 & V1_FAN_SUPPORTED) ? "YES" : "NO");
        printf("    VR_PARAMS_SUPPORTED:   %s\n", (data0 & VR_PARAMS_SUPPORTED) ? "YES" : "NO");
        printf("    V1_FAN_PROVISIONED:    %s\n", (data0 & V1_FAN_PROVISIONED) ? "YES" : "NO");
        printf("    VR_PARAMS_PROVISIONED: %s\n", (data0 & VR_PARAMS_PROVISIONED) ? "YES" : "NO");
    } else {
        printf("  Capability Status: READ FAILED\n");
    }

    /* Fan firmware version */
    printf("\n  Fan Controller Firmware:\n");
    ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_LOW, FAN_TABLE, &data0, NULL);
    if (ret == 0) {
        printf("    Version: %u.%u", (data0 >> 16) & 0xFFFF, data0 & 0xFFFF);
        ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_HIGH, FAN_TABLE, &data1, NULL);
        if (ret == 0) {
            printf(".%u.%u\n", (data1 >> 16) & 0xFFFF, data1 & 0xFFFF);
        } else {
            printf("\n");
        }
    } else {
        printf("    Version: READ FAILED\n");
    }

    /* VR config firmware version */
    printf("\n  VR Config Firmware:\n");
    ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_LOW, VR_CONFIG, &data0, NULL);
    if (ret == 0) {
        printf("    Version: %u.%u", (data0 >> 16) & 0xFFFF, data0 & 0xFFFF);
        ret = pcode_read(PCODE_LATE_BINDING, GET_VERSION_HIGH, VR_CONFIG, &data1, NULL);
        if (ret == 0) {
            printf(".%u.%u\n", (data1 >> 16) & 0xFFFF, data1 & 0xFFFF);
        } else {
            printf("\n");
        }
    } else {
        printf("    Version: READ FAILED (may not be provisioned)\n");
    }

    printf("\n");
    return 0;
}

static int cmd_write_duty(int duty, int force)
{
    uint32_t data0, data1;
    int ret;

    if (duty < 0 || duty > 255) {
        fprintf(stderr, "Duty must be 0-255, got %d\n", duty);
        return -1;
    }

    printf("\n=== Attempting Fan Duty Write (duty=%d/255 = %.1f%%) ===\n\n",
           duty, (duty * 100.0) / 255.0);

    if (!force) {
        printf("  [DRY RUN] Use --force to actually send write commands.\n");
        printf("  Will probe which subcommands accept write data...\n\n");
    }

    /* Read current fan speed first */
    printf("  Current fan speed: ");
    ret = pcode_read(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0, &data0, NULL);
    if (ret == 0) {
        printf("%u fans detected\n", data0);
    } else {
        printf("read failed\n");
    }

    /* Try writing to each undocumented subcommand */
    for (uint32_t sub = 0; sub <= FSC_PROBE_MAX; sub++) {
        if (sub == FSC_READ_NUM_FANS)
            continue;

        print_ts();
        printf("  [0x%x] ", sub);

        if (force) {
            /* Actually write the duty value */
            ret = pcode_write(FAN_SPEED_CONTROL, sub, 0, (uint32_t)duty, 0);
            if (ret == 0) {
                printf("WRITE SUCCESS (duty=%d) *** CANDIDATE FOUND! ***\n", duty);

                /* Read back to verify */
                ret = pcode_read(FAN_SPEED_CONTROL, sub, 0, &data0, &data1);
                if (ret == 0) {
                    printf("         Readback: data0=0x%08x data1=0x%08x\n", data0, data1);
                }
            } else {
                uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
                printf("write failed: %s (0x%02x)\n", pcode_err_str(status), status);
            }
        } else {
            /* Dry run: just read to see what the subcommand returns */
            ret = pcode_read(FAN_SPEED_CONTROL, sub, 0, &data0, &data1);
            if (ret == 0) {
                printf("READ OK data0=0x%08x data1=0x%08x (potential write target)\n",
                       data0, data1);
            } else {
                uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
                printf("read failed: %s (0x%02x)\n", pcode_err_str(status), status);
            }
        }
    }

    printf("\n");
    return 0;
}

static int cmd_interactive(void)
{
    char line[256];
    uint32_t cmd, p1, p2, d0, d1;
    int ret;
    uint32_t data0, data1;

    printf("\n=== Interactive PCODE Mailbox ===\n");
    printf("Format: R cmd param1 param2        (read)\n");
    printf("        W cmd param1 param2 data0  (write)\n");
    printf("        W64 cmd param1 param2 d0 d1 (write64)\n");
    printf("        Q to quit\n\n");

    while (1) {
        printf("pcode> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        /* Strip newline */
        line[strcspn(line, "\n")] = 0;

        if (line[0] == 'Q' || line[0] == 'q')
            break;

        if (line[0] == 'R' || line[0] == 'r') {
            if (sscanf(line + 1, " %x %x %x", &cmd, &p1, &p2) != 3) {
                printf("  Usage: R <cmd_hex> <param1_hex> <param2_hex>\n");
                continue;
            }
            print_ts();
            ret = pcode_read(cmd, p1, p2, &data0, &data1);
            if (ret == 0) {
                printf("  READ OK: cmd=0x%02x p1=0x%02x p2=0x%02x -> data0=0x%08x data1=0x%08x\n",
                       cmd, p1, p2, data0, data1);
            } else {
                uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
                printf("  READ FAILED: %s (0x%02x)\n", pcode_err_str(status), status);
            }
        } else if (line[0] == 'W' || line[0] == 'w') {
            if ((line[0] == 'W' && line[1] == '6' && line[2] == '4') ||
                (line[0] == 'w' && line[1] == '6' && line[2] == '4')) {
                if (sscanf(line + 3, " %x %x %x %x %x", &cmd, &p1, &p2, &d0, &d1) != 5) {
                    printf("  Usage: W64 <cmd> <p1> <p2> <data0> <data1>\n");
                    continue;
                }
            } else {
                if (sscanf(line + 1, " %x %x %x %x", &cmd, &p1, &p2, &d0) != 4) {
                    printf("  Usage: W <cmd> <param1> <param2> <data0>\n");
                    continue;
                }
                d1 = 0;
            }

            print_ts();
            printf("  Writing: cmd=0x%02x p1=0x%02x p2=0x%02x data0=0x%08x data1=0x%08x\n",
                   cmd, p1, p2, d0, d1);

            ret = pcode_write(cmd, p1, p2, d0, d1);
            if (ret == 0) {
                printf("  WRITE OK\n");
                /* Read back data registers */
                data0 = mmio_read32(PCODE_DATA0_OFFSET);
                data1 = mmio_read32(PCODE_DATA1_OFFSET);
                printf("  Response: data0=0x%08x data1=0x%08x\n", data0, data1);
            } else {
                uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
                printf("  WRITE FAILED: %s (0x%02x)\n", pcode_err_str(status), status);
            }
        } else if (line[0] == 'M' || line[0] == 'm') {
            /* Raw MMIO read */
            uint32_t offset;
            if (sscanf(line + 1, " %x", &offset) != 1) {
                printf("  Usage: M <offset_hex>\n");
                continue;
            }
            if (offset >= BAR0_SIZE) {
                printf("  Offset out of range (max 0x%x)\n", BAR0_SIZE - 1);
                continue;
            }
            data0 = mmio_read32(offset);
            printf("  MMIO[0x%06x] = 0x%08x\n", offset, data0);
        } else {
            printf("  Unknown command. Use R, W, W64, M, or Q.\n");
        }
    }
    return 0;
}

static void usage(const char *prog)
{
    printf("xe_pcode_probe - Intel Arc B580 PCODE Mailbox Probe Tool\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --probe           Probe all FAN_SPEED_CONTROL subcommands\n");
    printf("  --read-fan        Read fan RPM from tachometer registers\n");
    printf("  --read-temp       Read package and VRAM temperature\n");
    printf("  --late-bind       Query late binding firmware status\n");
    printf("  --write-duty N    Attempt to set fan duty (0-255)\n");
    printf("  --force           Actually send write commands (use with --write-duty)\n");
    printf("  --interactive     Interactive mailbox command sender\n");
    printf("  --verbose         Verbose output (show all errors)\n");
    printf("  --all             Run all read-only probes sequentially\n");
    printf("  --pci BDF         PCI address (e.g. 0000:09:00.0); auto-detects 8086:e20b\n");
    printf("  -h, --help        Show this help\n");
    printf("\nRequires root privileges (access to PCI resource files).\n");
    printf("Override PCI BDF with XE_PCI_BDF environment variable.\n");
}

int main(int argc, char **argv)
{
    int opt;
    int do_probe = 0, do_read_fan = 0, do_read_temp = 0;
    int do_late_bind = 0, do_interactive = 0, do_all = 0;
    int write_duty = -1, force = 0;

    static struct option long_opts[] = {
        {"probe",       no_argument,       0, 'p'},
        {"read-fan",    no_argument,       0, 'f'},
        {"read-temp",   no_argument,       0, 't'},
        {"late-bind",   no_argument,       0, 'l'},
        {"write-duty",  required_argument, 0, 'w'},
        {"force",       no_argument,       0, 'F'},
        {"interactive", no_argument,       0, 'i'},
        {"verbose",     no_argument,       0, 'v'},
        {"all",         no_argument,       0, 'a'},
        {"pci",         required_argument, 0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "hpftlw:Fivad:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': do_probe = 1; break;
        case 'f': do_read_fan = 1; break;
        case 't': do_read_temp = 1; break;
        case 'l': do_late_bind = 1; break;
        case 'w': write_duty = atoi(optarg); break;
        case 'F': force = 1; break;
        case 'i': do_interactive = 1; break;
        case 'v': verbose = 1; break;
        case 'a': do_all = 1; break;
        case 'd':
            snprintf(pci_bdf, sizeof(pci_bdf), "%s", optarg);
            break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (argc == 1) {
        usage(argv[0]);
        return 1;
    }

    /* Check if running as root */
    if (geteuid() != 0) {
        fprintf(stderr, "Error: must run as root to access PCI resource files\n");
        return 1;
    }

    /* Map BAR0 */
    printf("Intel Arc B580 PCODE Mailbox Probe Tool\n");
    printf("========================================\n");

    if (resolve_pci_bdf() < 0)
        return 1;
    printf("[INFO] Using PCI device %s (8086:e20b)\n", pci_bdf);

    if (map_bar0() < 0)
        return 1;

    /* Verify we can read the mailbox register */
    uint32_t mbx_test = mmio_read32(PCODE_MAILBOX_OFFSET);
    printf("[INFO] PCODE_MAILBOX initial value: 0x%08x (READY=%s)\n",
           mbx_test, (mbx_test & PCODE_READY) ? "YES" : "NO");

    int ret = 0;

    if (do_all) {
        do_probe = 1;
        do_read_fan = 1;
        do_read_temp = 1;
        do_late_bind = 1;
    }

    if (do_probe)    ret |= cmd_probe();
    if (do_read_fan) ret |= cmd_read_fan();
    if (do_read_temp) ret |= cmd_read_temp();
    if (do_late_bind) ret |= cmd_late_bind();
    if (write_duty >= 0) ret |= cmd_write_duty(write_duty, force);
    if (do_interactive)  ret |= cmd_interactive();

    unmap_bar0();
    printf("[INFO] BAR0 unmapped, cleanup complete.\n");
    return ret;
}
