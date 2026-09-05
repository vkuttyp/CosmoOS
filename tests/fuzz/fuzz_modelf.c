/*
 * fuzz_modelf.c - Module ELF validation (kernel/module/modelf.c) under
 * fuzzing (docs/verification/design.md). The seed is the synthetic image
 * test_modelf uses; any input must be validated without reading outside
 * it, and a layout the validator accepts must describe sections inside it.
 */

#include "fuzz.h"

#include "../host/modelf_image.h"

#include <kernel/errno.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct modelf_layout l;
    const char *why = NULL;
    memset(&l, 0, sizeof(l));
    int rc = modelf_validate(data, size, &l, &why);
    if (rc == 0) {
        /* Every section with file bytes lies inside the image, and so does
         * the metadata record the layout points at. */
        FUZZ_ASSERT(l.nr_sections <= MODELF_MAX_SECTIONS);
        for (unsigned i = 0; i < l.nr_sections; i++) {
            const struct modelf_section *s = &l.sections[i];
            FUZZ_ASSERT(s->group < MODELF_GROUPS);
            if (!s->nobits)
                FUZZ_ASSERT(s->file_off <= size && s->size <= size - s->file_off);
            FUZZ_ASSERT(s->offset <= l.group_size[s->group] && s->size <= l.group_size[s->group] - s->offset);
        }
        FUZZ_ASSERT(l.info_file_off <= size && sizeof(struct cosmo_module_info) <= size - l.info_file_off);
        modelf_check_info((const struct cosmo_module_info *)(data + l.info_file_off), &why);
    } else {
        FUZZ_ASSERT(rc == -ENOEXEC || rc == -EINVAL || rc == -ENOMEM);
        FUZZ_ASSERT(why != NULL);
    }
    if (size == sizeof(struct cosmo_module_info))
        modelf_check_info((const struct cosmo_module_info *)data, &why);
    return 0;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (i == 0) {
        struct image img = build_image();
        size_t n = img.size <= cap ? img.size : cap;
        memcpy(buf, img.bytes, n);
        free(img.bytes);
        return n;
    }
    if (i == 1) {
        /* A bare valid metadata record, for the standalone check. */
        struct cosmo_module_info info;
        memset(&info, 0, sizeof(info));
        memcpy(info.magic, COSMO_MODULE_MAGIC, 8);
        info.abi_version = COSMO_MODULE_ABI_VERSION;
        strcpy(info.name, "seed");
        strcpy(info.version, "1.0");
        if (sizeof(info) > cap)
            return 0;
        memcpy(buf, &info, sizeof(info));
        return sizeof(info);
    }
    return 0;
}
