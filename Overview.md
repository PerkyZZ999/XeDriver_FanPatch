# SPEC.md: Intel Arc B580 Linux Fan Control Implementation Project

## 1. Executive Summary & Core Problem Space

The **Intel Arc B580 (Battlemage)** GPU delivers excellent compute and graphics performance on Linux via the modern native **`xe` kernel driver**. However, user-space fan speed customization is currently broken or unimplemented upstream. While metrics like GPU temperature and fan RPM are readable through the kernel's hardware monitoring subsystem (`hwmon`), the ability to modify fan curves or force manual PWM targets is completely missing.
This document serves as a technical specification, project plan, and research roadmap for attempting a custom driver-level implementation using state-of-the-art Large Language Models (LLMs) to interface with the kernel architecture.

---

## 2. Host Machine Hardware Profile

The target environment for this implementation utilizes the following hardware configuration:

* **CPU:** AMD Ryzen 7 5800XT (8 Cores / 16 Threads)
* **GPU:** ASRock Challenger Arc B580 12GB GDDR6 (Running on the `xe` driver stack)
* **Motherboard:** GIGABYTE B550M Gaming X WiFi 6
* **Memory:** 40 GB DDR4 @ 3200 MT/s (Asymmetrical configuration: 3x 8GB + 1x 16GB)
* **Power Supply:** SAMA G650 650W (80 Plus Gold & Cybenetics Platinum Efficiency)
* **OS Context:** Modern Linux kernel architecture (Optimized for low-overhead Wayland compositions)

---

## 3. Structured Project Layout Phases

### Phase 1: Subsystem Auditing & Local Kernel Lab

Before writing any code, you must establish a secure development environment to build, patch, and test custom kernel modules without destabilizing your main system.

* **Task 1.1:** Clone the upstream graphics development tree (`drm-xe-next`) or the target long-term support branch.
* **Task 1.2:** Locate and isolate `drivers/gpu/drm/xe/xe_hwmon.c`. This is the exact entry point where the kernel registers hardware monitoring capabilities with `sysfs`.
* **Task 1.3:** Map out how the current driver handles `hwmon` attributes. Currently, it only exposes read privileges (`S_IRUGO`) for attributes like `fan1_input`.
  
  ### Phase 2: Sysfs Boilerplate Injection (LLM Orchestration)
  
  This phase uses a SOTA LLM to build the foundation for user-space communication. 
* **Task 2.1:** Feed `xe_hwmon.c` into the LLM and instruct it to refactor the macro declarations to include write permissions (`S_IWUSR`).
* **Task 2.2:** Have the LLM generate standard kernel store functions (`xe_hwmon_fan_store` or `xe_hwmon_pwm_store`) that capture string inputs from `/sys/class/hwmon/hwmonX/pwm1` and convert them into clean integers (0–255).
* **Task 2.3:** Implement rigorous bounds checking in the generated C code to prevent memory leaks or kernel panics from malformed user-space inputs.
  
  ### Phase 3: The Mailbox & Register Hunt (The Blocker)
  
  This is the exploratory phase where data abstraction ends and physical hardware control begins.
* **Task 3.1:** Analyze how the `xe` driver communicates with the GPU's onboard Power Management Unit (PMU) or GuC (Graphics Microcontroller).
* **Task 3.2:** Search the codebase for Mailbox register macros (`XE_REG_MBOX` or similar hardware abstract communication structures).
* **Task 3.3:** Use the LLM to write a basic telemetry logging routine inside the driver to spy on firmware calls when the GPU initializes its baseline default fan curve.
  
  ### Phase 4: Verification & Hardware Fallback
* **Task 4.1:** Compile the patched `xe.ko` module, inject it into the local kernel using `modprobe`, and check if `sysfs` successfully generates a writable `pwm1` handle.
* **Task 4.2 (Fallback Execution):** If the register offsets remain completely inaccessible due to closed hardware walls, execute the physical redirection loop: adapt the GPU shroud's mini-4pin fan line directly to an open header on the **GIGABYTE B550M** motherboard and control it cleanly via user-space tools like `coolercontrol`.

---

## 4. Technical Blockers & Critical Bottlenecks

```
+-------------------------------------------------------------+
|                     User Space (LACT / CLI)                 |
+-------------------------------------------------------------+
                               |  Writes to /sys/class/hwmon/
                               v
+-------------------------------------------------------------+
|            Kernel Space (xe_hwmon.c Writable Patch)         |
|   [LLM can perfectly code the Sysfs boilerplate here]      |
+-------------------------------------------------------------+
                               |  Attempts to forward command
                               v
+=============================================================+
|  BLOCKER: Missing Register Maps / Mailbox Commands          |
|  (Intel's undocumented firmware protocols for Battlemage)   |
+=============================================================+
                               |  Cannot Cross
                               v
+-------------------------------------------------------------+
|              Physical Hardware (Arc B580 Fan PMU)           |
+-------------------------------------------------------------+
```

1. **The Mailbox Command Black Box:** High-level software cannot talk directly to a fan motor. The kernel driver must write a specific hex value to an obscure hardware register offset. Because Battlemage is a fresh architecture, Intel hasn't published these register specifications. LLMs cannot deduce these values through pure logic; they must be provided by documentation or extracted via reverse engineering.
2. **Level Zero / Sysman Abstraction Deficit:** Compute interfaces rely on Level Zero APIs like `zesDeviceEnumFans()`. Currently, on Battlemage, this call returns a count of `0`. This proves that the underlying firmware driver is completely blind to fan controls, meaning the issue runs deeper than simple UI omissions.
3. **Kernel Panics during Testing:** Buggy code inside a kernel driver will immediately crash the operating system. Testing requires a secondary machine, an active SSH/serial debugging connection, or a robust VM pass-through framework to capture logs safely.

---

## 5. Targeted Research Roadmap

If you choose to pursue this engineering project with an LLM, focus your research and data collection on these exact channels:

* **Upstream Commit Scrape:** Frequently audit the `freedesktop.org` GitLab repository for the `drm/xe` driver. Look closely for commits containing keywords like `PMU`, `hwmon`, `fan`, `pwm`, or `MBOX`. Developers often push the low-level register headers months before completing the actual implementation features.
* **Legacy Driver Cross-Referencing:** Use the LLM to compare `xe_hwmon.c` with the older `drivers/gpu/drm/i915/i915_hwmon.c`. The `i915` driver has fully realized, functional fan controls for older Intel architectures. Mapping how `i915` structured its mailbox calls can provide structural blueprints for patching `xe`.
* **Kernel Mailing List (LKML) Monitoring:** Subscribe to or search the Intel-xe mailing list. Check if downstream distro maintainers or Intel open-source engineers have posted experimental patch series addressing Battlemage thermal controls. If an experimental patch is found, an LLM can effortlessly port and backport it to your running kernel branch.
