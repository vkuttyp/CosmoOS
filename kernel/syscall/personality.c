/*
 * personality.c - Which personality runs a program (roadmap M3,
 * docs/kernel/process/design.md, "Personalities").
 *
 * The one place that lists the personalities, so the process code names
 * none of them: a process the kernel creates is native; any other ELF
 * runs under the first personality that claims it -- native claims the
 * CosmoOS note, Linux takes every ELF without one.
 */

#include <kernel/elf.h>
#include <kernel/panic.h>
#include <kernel/process.h>

static const struct personality *const g_personalities[] = {
    &personality_native,
    &personality_linux,
};

const struct personality *personality_for_elf(const struct elf_info *info, bool kernel_created)
{
    if (kernel_created)
        return &personality_native;
    for (unsigned i = 0; i < sizeof(g_personalities) / sizeof(g_personalities[0]); i++) {
        const struct personality *pers = g_personalities[i];
        if (pers->claims_elf != NULL && pers->claims_elf(info))
            return pers;
    }
    panic("process: no personality claims an ELF the loader accepted");
}
