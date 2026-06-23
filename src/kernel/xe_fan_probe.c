// SPDX-License-Identifier: GPL-2.0
/*
 * xe_fan_probe - Out-of-tree Kernel Module for Intel Arc B580 Fan Control
 *
 * Probes PCODE FAN_SPEED_CONTROL mailbox subcommands and exposes
 * fan/temperature/PWM data via sysfs. Does not modify the running xe driver.
 *
 * Build: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 * Load:  sudo insmod xe_fan_probe.ko
 * Use:   cat /sys/class/xe_fan/probe_results
 *        cat /sys/class/xe_fan/fan1_input
 *        echo 128 > /sys/class/xe_fan/pwm1  (if write subcommand found)
 *
 * Copyright 2026 - XeDriver Fan Patch Project
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/ctype.h>

MODULE_AUTHOR("XeDriver Fan Patch Project");
MODULE_DESCRIPTION("Intel Arc B580 PCODE Fan Control Probe Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

/* === Register Definitions === */

#define BAR0_SIZE           (16 * 1024 * 1024)  /* 16MB */

#define PCODE_MAILBOX_OFFSET  0x138124
#define PCODE_DATA0_OFFSET    0x138128
#define PCODE_DATA1_OFFSET    0x13812C

#define BMG_FAN_1_SPEED_OFFSET  0x138140
#define BMG_FAN_2_SPEED_OFFSET  0x138170
#define BMG_FAN_3_SPEED_OFFSET  0x1381a0

#define BMG_PKG_TEMP_OFFSET   0x138434

/* PCODE_MAILBOX register fields */
#define PCODE_READY           (1u << 31)
#define PCODE_ERROR_MASK      0x000000FFu

/* PCODE commands */
#define PCODE_POWER_SETUP     0x7C
#define PCODE_THERMAL_INFO    0x25
#define PCODE_LATE_BINDING    0x5C
#define FAN_SPEED_CONTROL     0x7D

/* FAN_SPEED_CONTROL subcommands */
#define FSC_READ_NUM_FANS    0x4
#define FSC_PROBE_MAX        0xF

/* PCODE_LATE_BINDING subcommands */
#define GET_CAPABILITY_STATUS  0x0
#define GET_VERSION_LOW        0x1
#define GET_VERSION_HIGH       0x2

/* Late binding capability bits */
#define V1_FAN_SUPPORTED       (1u << 0)
#define V1_FAN_PROVISIONED     (1u << 16)

#define FAN_TABLE              1

/* PCODE error codes */
#define PCODE_SUCCESS              0x0
#define PCODE_ILLEGAL_CMD          0x1
#define PCODE_ILLEGAL_SUBCOMMAND   0x4
#define PCODE_LOCKED               0x6

/* Mailbox composition macro */
#define PCODE_MBOX(cmd, p1, p2) \
    (((u32)(cmd) & 0xFF) | (((u32)(p1) & 0xFF) << 8) | (((u32)(p2) & 0xFF) << 16))

#define PCODE_TIMEOUT_US    1000  /* 1ms */
#define BMG_FAN_MAX         3
#define MAX_PROBE_RESULTS   256

/* === Module state === */

static struct pci_dev *gpu_pdev = NULL;
static void __iomem *mmio_base = NULL;
static DEFINE_SPINLOCK(pcode_lock);
static struct kobject *fan_kobj = NULL;

/* Probe results storage */
struct fsc_probe_result {
    u8 subcmd;
    int success;
    u32 data0;
    u32 data1;
    u32 error_code;
};

static struct fsc_probe_result probe_results[FSC_PROBE_MAX + 1];
static int probe_count = 0;
static u32 fan_count = 0;
static u32 lb_cap_status = 0;
static u32 lb_fan_version_major = 0;
static u32 lb_fan_version_minor = 0;
static u32 lb_fan_version_hotfix = 0;
static u32 lb_fan_version_build = 0;
static int fsc_write_subcmd = -1;  /* Discovered write subcommand (-1 = not found) */
static int pwm_manual_mode = 0;    /* 0 = auto/firmware, 1 = manual */
static u32 current_pwm = 0;        /* Current PWM duty (0-255) */

/* === MMIO access helpers === */

static inline u32 mmio_read32(u32 offset)
{
    return ioread32(mmio_base + offset);
}

static inline void mmio_write32(u32 offset, u32 val)
{
    iowrite32(val, mmio_base + offset);
}

/* === PCODE Mailbox Protocol === */

static const char *pcode_err_str(u32 err)
{
    switch (err) {
    case PCODE_SUCCESS:            return "Success";
    case PCODE_ILLEGAL_CMD:        return "Illegal Command";
    case 0x2:                      return "Timeout";
    case 0x3:                      return "Illegal Data";
    case PCODE_ILLEGAL_SUBCOMMAND: return "Illegal Subcommand";
    case PCODE_LOCKED:             return "Locked";
    case 0x10:                     return "GT ratio out of range";
    case 0x11:                     return "Rejected";
    default:                       return "Unknown";
    }
}

static int wait_pcode_ready(u32 timeout_us)
{
    u32 elapsed = 0;
    u32 step = 10;

    while (elapsed < timeout_us) {
        if ((mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) == 0)
            return 0;
        udelay(step);
        elapsed += step;
    }
    return -ETIMEDOUT;
}

static int pcode_mailbox_read(u32 cmd, u32 p1, u32 p2, u32 *d0, u32 *d1)
{
    unsigned long flags;
    u32 mbox;
    int ret;

    spin_lock_irqsave(&pcode_lock, flags);

    /* Wait if mailbox is busy */
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) {
            pr_err("PCODE mailbox busy (READY stuck)\n");
            goto out;
        }
    }

    /* Clear data registers */
    mmio_write32(PCODE_DATA0_OFFSET, 0);
    mmio_write32(PCODE_DATA1_OFFSET, 0);
    wmb();

    /* Send command */
    mbox = PCODE_READY | PCODE_MBOX(cmd, p1, p2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);
    wmb();

    /* Wait for completion */
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) {
        pr_err("PCODE read timeout (cmd=0x%02x p1=0x%02x p2=0x%02x)\n", cmd, p1, p2);
        goto out;
    }

    /* Read response */
    rmb();
    *d0 = mmio_read32(PCODE_DATA0_OFFSET);
    if (d1)
        *d1 = mmio_read32(PCODE_DATA1_OFFSET);

    /* Check errors */
    u32 status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS) {
        pr_debug("PCODE cmd=0x%02x p1=0x%02x p2=0x%02x: %s (0x%02x)\n",
                 cmd, p1, p2, pcode_err_str(status), status);
        ret = -EIO;
        /* Store error code for probe results */
        if (d1)
            *d1 = status;
    }

out:
    spin_unlock_irqrestore(&pcode_lock, flags);
    return ret;
}

static int pcode_mailbox_write(u32 cmd, u32 p1, u32 p2, u32 d0, u32 d1)
{
    unsigned long flags;
    u32 mbox;
    int ret;

    spin_lock_irqsave(&pcode_lock, flags);

    /* Wait if mailbox is busy */
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) {
            pr_err("PCODE mailbox busy\n");
            goto out;
        }
    }

    /* Write data */
    mmio_write32(PCODE_DATA0_OFFSET, d0);
    mmio_write32(PCODE_DATA1_OFFSET, d1);
    wmb();

    /* Send command */
    mbox = PCODE_READY | PCODE_MBOX(cmd, p1, p2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);
    wmb();

    /* Wait for completion */
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) {
        pr_err("PCODE write timeout\n");
        goto out;
    }

    /* Check errors */
    rmb();
    u32 status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS) {
        pr_err("PCODE write cmd=0x%02x p1=0x%02x p2=0x%02x: %s (0x%02x)\n",
               cmd, p1, p2, pcode_err_str(status), status);
        ret = -EIO;
    }

out:
    spin_unlock_irqrestore(&pcode_lock, flags);
    return ret;
}

/* === Fan reading === */

static int read_fan_rpm(int channel, long *rpm)
{
    static u32 prev_tach[BMG_FAN_MAX] = {0};
    static u64 prev_jiffies = 0;
    u32 offsets[BMG_FAN_MAX] = {
        BMG_FAN_1_SPEED_OFFSET,
        BMG_FAN_2_SPEED_OFFSET,
        BMG_FAN_3_SPEED_OFFSET
    };
    u32 curr_tach, pulses, rotations;
    u64 now_jiffies;
    u64 elapsed_ms;

    if (channel < 0 || channel >= BMG_FAN_MAX)
        return -EINVAL;

    curr_tach = mmio_read32(offsets[channel]);
    now_jiffies = get_jiffies_64();
    elapsed_ms = jiffies_delta_to_msecs(now_jiffies - prev_jiffies);

    if (elapsed_ms == 0) {
        *rpm = 0;
        return -EAGAIN;
    }

    pulses = curr_tach - prev_tach[channel];
    rotations = pulses / 2;
    *rpm = div_u64(rotations * (MSEC_PER_SEC * 60), elapsed_ms);

    prev_tach[channel] = curr_tach;
    if (channel == 0)
        prev_jiffies = now_jiffies;

    return 0;
}

/* === Subcommand probing === */

static void probe_fsc_subcommands(void)
{
    u32 d0, d1;
    int ret;
    int i;

    pr_info("Probing FAN_SPEED_CONTROL (0x7D) subcommands...\n");
    probe_count = 0;

    /* Known: FSC_READ_NUM_FANS (0x4) */
    ret = pcode_mailbox_read(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0, &d0, NULL);
    if (ret == 0) {
        fan_count = d0;
        pr_info("  FSC_READ_NUM_FANS (0x4): SUCCESS, fan_count=%u\n", fan_count);

        probe_results[probe_count].subcmd = FSC_READ_NUM_FANS;
        probe_results[probe_count].success = 1;
        probe_results[probe_count].data0 = d0;
        probe_results[probe_count].data1 = 0;
        probe_results[probe_count].error_code = 0;
        probe_count++;
    } else {
        fan_count = 1;
        pr_warn("  FSC_READ_NUM_FANS (0x4): FAILED, assuming 1 fan\n");
    }

    /* Probe undocumented subcommands */
    for (i = 0; i <= FSC_PROBE_MAX; i++) {
        if (i == FSC_READ_NUM_FANS)
            continue;

        ret = pcode_mailbox_read(FAN_SPEED_CONTROL, i, 0, &d0, &d1);

        probe_results[probe_count].subcmd = (u8)i;
        if (ret == 0) {
            probe_results[probe_count].success = 1;
            probe_results[probe_count].data0 = d0;
            probe_results[probe_count].data1 = d1;
            probe_results[probe_count].error_code = 0;
            pr_info("  FSC subcmd 0x%x: SUCCESS, data0=0x%08x data1=0x%08x\n",
                    i, d0, d1);
        } else {
            probe_results[probe_count].success = 0;
            probe_results[probe_count].data0 = 0;
            probe_results[probe_count].data1 = 0;
            probe_results[probe_count].error_code = (ret == -EIO) ?
                (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK) : 0xFF;
            pr_debug("  FSC subcmd 0x%x: FAILED (%s)\n",
                     i, pcode_err_str(probe_results[probe_count].error_code));
        }
        probe_count++;
    }

    pr_info("Probe complete: %d/%d subcommands succeeded\n",
            probe_count - 1, FSC_PROBE_MAX);
}

static void query_late_binding(void)
{
    u32 d0, d1;
    int ret;

    ret = pcode_mailbox_read(PCODE_LATE_BINDING, GET_CAPABILITY_STATUS, 0, &d0, NULL);
    if (ret == 0) {
        lb_cap_status = d0;
        pr_info("Late binding capability: 0x%08x", lb_cap_status);
        pr_cont(" V1_FAN_SUPPORTED=%s V1_FAN_PROVISIONED=%s\n",
                (lb_cap_status & V1_FAN_SUPPORTED) ? "YES" : "NO",
                (lb_cap_status & V1_FAN_PROVISIONED) ? "YES" : "NO");
    }

    ret = pcode_mailbox_read(PCODE_LATE_BINDING, GET_VERSION_LOW, FAN_TABLE, &d0, NULL);
    if (ret == 0) {
        lb_fan_version_major = (d0 >> 16) & 0xFFFF;
        lb_fan_version_minor = d0 & 0xFFFF;

        ret = pcode_mailbox_read(PCODE_LATE_BINDING, GET_VERSION_HIGH, FAN_TABLE, &d1, NULL);
        if (ret == 0) {
            lb_fan_version_hotfix = (d1 >> 16) & 0xFFFF;
            lb_fan_version_build = d1 & 0xFFFF;
        }
        pr_info("Fan controller firmware: v%u.%u.%u.%u\n",
                lb_fan_version_major, lb_fan_version_minor,
                lb_fan_version_hotfix, lb_fan_version_build);
    }
}

/* === Sysfs attributes === */

static ssize_t probe_results_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf)
{
    int len = 0;
    int i;

    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "FAN_SPEED_CONTROL (0x7D) Subcommand Probe Results\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "================================================\n\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Fan count: %u\n\n", fan_count);

    for (i = 0; i < probe_count; i++) {
        if (probe_results[i].success) {
            len += scnprintf(buf + len, PAGE_SIZE - len,
                             "  [0x%x] SUCCESS  data0=0x%08x data1=0x%08x\n",
                             probe_results[i].subcmd,
                             probe_results[i].data0,
                             probe_results[i].data1);
        } else {
            len += scnprintf(buf + len, PAGE_SIZE - len,
                             "  [0x%x] FAILED   error=%s (0x%02x)\n",
                             probe_results[i].subcmd,
                             pcode_err_str(probe_results[i].error_code),
                             probe_results[i].error_code);
        }
    }

    len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Late Binding Status:\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "  V1_FAN_SUPPORTED: %s\n",
                     (lb_cap_status & V1_FAN_SUPPORTED) ? "YES" : "NO");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "  V1_FAN_PROVISIONED: %s\n",
                     (lb_cap_status & V1_FAN_PROVISIONED) ? "YES" : "NO");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "  Fan FW Version: %u.%u.%u.%u\n\n",
                     lb_fan_version_major, lb_fan_version_minor,
                     lb_fan_version_hotfix, lb_fan_version_build);

    if (fsc_write_subcmd >= 0) {
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "Write subcommand FOUND: 0x%x\n", fsc_write_subcmd);
    } else {
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "Write subcommand: NOT FOUND (use --write-duty probe)\n");
    }

    return len;
}

static ssize_t fan_input_show(struct kobject *kobj,
                               struct kobj_attribute *attr, char *buf)
{
    long rpm = 0;
    int channel = 0;

    /* Determine channel from attribute name */
    const char *name = attr->attr.name;
    if (strstr(name, "fan2"))
        channel = 1;
    else if (strstr(name, "fan3"))
        channel = 2;

    if (channel >= (int)fan_count)
        return scnprintf(buf, PAGE_SIZE, "0\n");

    read_fan_rpm(channel, &rpm);
    return scnprintf(buf, PAGE_SIZE, "%ld\n", rpm);
}

static ssize_t temp_input_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    u32 raw = mmio_read32(BMG_PKG_TEMP_OFFSET);
    u32 temp = raw & 0xFF;
    return scnprintf(buf, PAGE_SIZE, "%u\n", temp * 1000); /* millidegrees */
}

static ssize_t pwm_show(struct kobject *kobj,
                         struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%u\n", current_pwm);
}

static ssize_t pwm_store(struct kobject *kobj,
                          struct kobj_attribute *attr,
                          const char *buf, size_t count)
{
    u32 duty;
    int ret;

    if (kstrtou32(buf, 0, &duty) < 0)
        return -EINVAL;

    if (duty > 255)
        return -EINVAL;

    if (fsc_write_subcmd < 0) {
        pr_err("No FSC write subcommand discovered. Cannot set PWM.\n");
        pr_err("Run userspace probe with --write-duty --force to find one.\n");
        return -EOPNOTSUPP;
    }

    pr_info("Setting fan duty to %u/255 via subcmd 0x%x\n",
            duty, fsc_write_subcmd);

    ret = pcode_mailbox_write(FAN_SPEED_CONTROL, fsc_write_subcmd, 0, duty, 0);
    if (ret) {
        pr_err("Fan duty write failed: %d\n", ret);
        return ret;
    }

    current_pwm = duty;
    pwm_manual_mode = 1;
    return count;
}

static ssize_t pwm_enable_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", pwm_manual_mode ? 1 : 0);
}

static ssize_t pwm_enable_store(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    int val;

    if (kstrtoint(buf, 0, &val) < 0)
        return -EINVAL;

    if (val == 0) {
        /* Return to auto/firmware control */
        pwm_manual_mode = 0;
        if (fsc_write_subcmd >= 0) {
            /* Write 0 to disable manual override (firmware resumes control) */
            pcode_mailbox_write(FAN_SPEED_CONTROL, fsc_write_subcmd, 0, 0, 0);
        }
        current_pwm = 0;
        pr_info("Fan control returned to firmware auto mode\n");
    } else if (val == 1) {
        pwm_manual_mode = 1;
        pr_info("Fan control set to manual mode\n");
    } else {
        return -EINVAL;
    }

    return count;
}

static ssize_t write_subcmd_show(struct kobject *kobj,
                                  struct kobj_attribute *attr, char *buf)
{
    if (fsc_write_subcmd < 0)
        return scnprintf(buf, PAGE_SIZE, "-1 (not found)\n");
    return scnprintf(buf, PAGE_SIZE, "0x%x\n", fsc_write_subcmd);
}

static ssize_t write_subcmd_store(struct kobject *kobj,
                                   struct kobj_attribute *attr,
                                   const char *buf, size_t count)
{
    int val;

    if (kstrtoint(buf, 0, &val) < 0)
        return -EINVAL;

    if (val < 0 || val > 0xFF)
        return -EINVAL;

    fsc_write_subcmd = val;
    pr_info("FSC write subcommand manually set to 0x%x\n", val);
    return count;
}

static ssize_t info_show(struct kobject *kobj,
                          struct kobj_attribute *attr, char *buf)
{
    int len = 0;

    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Intel Arc B580 Fan Control Module\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "==================================\n\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "GPU: Intel Battlemage G21 (Arc B580)\n");
    if (gpu_pdev)
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "PCI: %s (8086:e20b)\n", pci_name(gpu_pdev));
    else
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "PCI: (not bound)\n");
    if (gpu_pdev)
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "BAR0: 0x%llx (16MB)\n\n",
                         (u64)pci_resource_start(gpu_pdev, 0));
    else
        len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Fan count: %u\n", fan_count);
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Manual mode: %s\n", pwm_manual_mode ? "YES" : "NO");
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Current PWM: %u/255\n", current_pwm);
    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "Write subcmd: ");
    if (fsc_write_subcmd >= 0)
        len += scnprintf(buf + len, PAGE_SIZE - len, "0x%x\n", fsc_write_subcmd);
    else
        len += scnprintf(buf + len, PAGE_SIZE - len, "not found\n");

    return len;
}

/* Sysfs attribute definitions */
static struct kobj_attribute attr_probe_results =
    __ATTR_RO(probe_results);

static struct kobj_attribute attr_fan1_input =
    __ATTR(fan1_input, 0444, fan_input_show, NULL);

static struct kobj_attribute attr_fan2_input =
    __ATTR(fan2_input, 0444, fan_input_show, NULL);

static struct kobj_attribute attr_fan3_input =
    __ATTR(fan3_input, 0444, fan_input_show, NULL);

static struct kobj_attribute attr_temp1_input =
    __ATTR(temp1_input, 0444, temp_input_show, NULL);

static struct kobj_attribute attr_pwm1 =
    __ATTR(pwm1, 0644, pwm_show, pwm_store);

static struct kobj_attribute attr_pwm1_enable =
    __ATTR(pwm1_enable, 0644, pwm_enable_show, pwm_enable_store);

static struct kobj_attribute attr_write_subcmd =
    __ATTR(write_subcmd, 0644, write_subcmd_show, write_subcmd_store);

static struct kobj_attribute attr_info =
    __ATTR_RO(info);

static struct attribute *fan_attrs[] = {
    &attr_probe_results.attr,
    &attr_fan1_input.attr,
    &attr_fan2_input.attr,
    &attr_fan3_input.attr,
    &attr_temp1_input.attr,
    &attr_pwm1.attr,
    &attr_pwm1_enable.attr,
    &attr_write_subcmd.attr,
    &attr_info.attr,
    NULL,
};

static struct attribute_group fan_attr_group = {
    .attrs = fan_attrs,
};

/* === Module init/exit === */

static int __init xe_fan_probe_init(void)
{
    int ret;

    pr_info("Loading Intel Arc B580 Fan Control Probe Module\n");

    gpu_pdev = pci_get_device(0x8086, 0xe20b, NULL);
    if (!gpu_pdev) {
        pr_err("Intel Arc B580 (8086:e20b) not found on PCI bus\n");
        return -ENODEV;
    }

    ret = pci_enable_device(gpu_pdev);
    if (ret) {
        pr_err("pci_enable_device failed: %d\n", ret);
        goto err_put_dev;
    }

    mmio_base = pci_iomap(gpu_pdev, 0, BAR0_SIZE);
    if (!mmio_base) {
        pr_err("Failed to iomap PCI BAR0 for %s\n", pci_name(gpu_pdev));
        ret = -ENOMEM;
        goto err_disable;
    }
    pr_info("BAR0 mapped for %s\n", pci_name(gpu_pdev));

    /* Verify mailbox is accessible */
    u32 mbx = mmio_read32(PCODE_MAILBOX_OFFSET);
    pr_info("PCODE_MAILBOX initial: 0x%08x (READY=%s)\n",
            mbx, (mbx & PCODE_READY) ? "YES" : "NO");

    /* Probe FSC subcommands */
    probe_fsc_subcommands();

    /* Query late binding status */
    query_late_binding();

    /* Initialize fan tachometer baseline */
    {
        u32 tach = mmio_read32(BMG_FAN_1_SPEED_OFFSET);
        pr_info("Fan 1 tach baseline: %u\n", tach);
    }

    /* Create sysfs interface */
    fan_kobj = kobject_create_and_add("xe_fan", kernel_kobj);
    if (!fan_kobj) {
        pr_err("Failed to create sysfs kobject\n");
        ret = -ENOMEM;
        goto err_iounmap;
    }

    ret = sysfs_create_group(fan_kobj, &fan_attr_group);
    if (ret) {
        pr_err("Failed to create sysfs group: %d\n", ret);
        kobject_put(fan_kobj);
        fan_kobj = NULL;
        goto err_iounmap;
    }

    pr_info("Module loaded. Access via /sys/kernel/xe_fan/\n");
    pr_info("  cat /sys/kernel/xe_fan/probe_results\n");
    pr_info("  cat /sys/kernel/xe_fan/fan1_input\n");
    pr_info("  cat /sys/kernel/xe_fan/temp1_input\n");

    return 0;

err_iounmap:
    pci_iounmap(gpu_pdev, mmio_base);
    mmio_base = NULL;
err_disable:
    pci_disable_device(gpu_pdev);
err_put_dev:
    pci_dev_put(gpu_pdev);
    gpu_pdev = NULL;
    return ret;
}

static void __exit xe_fan_probe_exit(void)
{
    /* Return fan to auto mode if we were in manual */
    if (pwm_manual_mode && fsc_write_subcmd >= 0) {
        pcode_mailbox_write(FAN_SPEED_CONTROL, fsc_write_subcmd, 0, 0, 0);
        pr_info("Returned fan to firmware auto mode\n");
    }

    if (fan_kobj) {
        sysfs_remove_group(fan_kobj, &fan_attr_group);
        kobject_put(fan_kobj);
        fan_kobj = NULL;
    }

    if (mmio_base && gpu_pdev) {
        pci_iounmap(gpu_pdev, mmio_base);
        mmio_base = NULL;
    }

    if (gpu_pdev) {
        pci_disable_device(gpu_pdev);
        pci_dev_put(gpu_pdev);
        gpu_pdev = NULL;
    }

    pr_info("Module unloaded\n");
}

module_init(xe_fan_probe_init);
module_exit(xe_fan_probe_exit);
