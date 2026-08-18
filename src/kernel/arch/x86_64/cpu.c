/*
 * Qira OS - CPU identification
 *
 * Reads CPUID and records what the rest of the kernel needs to know about the
 * processor. The results are surfaced by `qcsh cpuinfo` and the System
 * Information application.
 */

#include <kernel/cpu.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/string.h>

static struct cpu_info info;

/* CPUID.1:EDX feature bits */
#define EDX_FPU   (1u << 0)
#define EDX_TSC   (1u << 4)
#define EDX_MSR   (1u << 5)
#define EDX_PAE   (1u << 6)
#define EDX_APIC  (1u << 9)
#define EDX_MMX   (1u << 23)
#define EDX_SSE   (1u << 25)
#define EDX_SSE2  (1u << 26)

/* CPUID.80000001:EDX */
#define EXT_NX    (1u << 20)
#define EXT_LM    (1u << 29)

void cpu_detect(void)
{
    uint32_t a, b, c, d;

    memset(&info, 0, sizeof(info));

    cpuid_raw(0, 0, &a, &b, &c, &d);
    info.max_leaf = a;
    memcpy(info.vendor + 0, &b, 4);
    memcpy(info.vendor + 4, &d, 4);
    memcpy(info.vendor + 8, &c, 4);
    info.vendor[12] = '\0';

    if (info.max_leaf >= 1) {
        cpuid_raw(1, 0, &a, &b, &c, &d);
        info.stepping     = a & 0x0F;
        info.model        = (a >> 4) & 0x0F;
        info.family       = (a >> 8) & 0x0F;
        if (info.family == 0x0F) {
            info.family += (a >> 20) & 0xFF;
        }
        if (info.family == 0x06 || info.family == 0x0F) {
            info.model += ((a >> 16) & 0x0F) << 4;
        }
        info.features_edx = d;
        info.features_ecx = c;
        info.has_apic     = (d & EDX_APIC) != 0;
        info.has_sse      = (d & EDX_SSE) != 0;
        info.has_sse2     = (d & EDX_SSE2) != 0;
        info.has_tsc      = (d & EDX_TSC) != 0;
        info.has_msr      = (d & EDX_MSR) != 0;
    }

    cpuid_raw(0x80000000u, 0, &a, &b, &c, &d);
    info.max_ext_leaf = a;

    if (info.max_ext_leaf >= 0x80000001u) {
        cpuid_raw(0x80000001u, 0, &a, &b, &c, &d);
        info.features_ext  = d;
        info.has_nx        = (d & EXT_NX) != 0;
        info.has_long_mode = (d & EXT_LM) != 0;
    }

    /* Brand string lives in leaves 0x80000002-0x80000004. */
    if (info.max_ext_leaf >= 0x80000004u) {
        uint32_t *brand = (uint32_t *)info.brand;
        for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid_raw(leaf, 0, &a, &b, &c, &d);
            *brand++ = a;
            *brand++ = b;
            *brand++ = c;
            *brand++ = d;
        }
        info.brand[48] = '\0';
    } else {
        strlcpy(info.brand, "Unknown x86-64 processor", sizeof(info.brand));
    }

    if (info.max_ext_leaf >= 0x80000008u) {
        cpuid_raw(0x80000008u, 0, &a, &b, &c, &d);
        info.phys_addr_bits = a & 0xFF;
        info.virt_addr_bits = (a >> 8) & 0xFF;
    } else {
        info.phys_addr_bits = 36;
        info.virt_addr_bits = 48;
    }

    /* Trim leading spaces that Intel pads the brand string with. */
    char *brand = info.brand;
    while (*brand == ' ') {
        brand++;
    }
    if (brand != info.brand) {
        memmove(info.brand, brand, strlen(brand) + 1);
    }

    KLOG_INFO("cpu", "%s", info.brand);
    KLOG_INFO("cpu", "vendor=%s family=%u model=%u stepping=%u", info.vendor,
              info.family, info.model, info.stepping);
    KLOG_INFO("cpu", "features:%s%s%s%s%s phys=%u-bit virt=%u-bit",
              info.has_sse ? " sse" : "", info.has_sse2 ? " sse2" : "",
              info.has_apic ? " apic" : "", info.has_nx ? " nx" : "",
              info.has_tsc ? " tsc" : "", info.phys_addr_bits,
              info.virt_addr_bits);
}

const struct cpu_info *cpu_get_info(void)
{
    return &info;
}
