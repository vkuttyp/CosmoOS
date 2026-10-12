/*
 * cosmomanifest.h - The signed boot manifest, \cosmo\manifest (roadmap M5;
 * docs/boot/design.md, "Verified boot").
 *
 * The build lists the SHA-512 and size of the three files the loader reads
 * -- kernel.elf, boot.tar, cmdline, in that order -- and signs the list with
 * one Ed25519 signature in the module-signing trailer (scripts/modsign.py,
 * docs/kernel/module/design.md): the payload below, then
 *
 *     uint8_t  sig[64];      Ed25519 over the payload
 *     uint8_t  key_id[8];    first 8 bytes of SHA-512(public key)
 *     uint32_t version;      COSMO_MANIFEST_SIG_VERSION
 *     uint32_t algo;         COSMO_MANIFEST_SIG_ED25519
 *     char     magic[8];     "COSMOSIG"
 *
 * so the file is exactly COSMO_MANIFEST_FILE_SIZE bytes. Written by
 * scripts/mkmanifest.py, checked by the UEFI loader (boot/uefi/verify.c)
 * against the keys compiled into it. Little-endian, fixed layout, no
 * pointers; any change bumps COSMO_MANIFEST_VERSION.
 */

#ifndef COSMOMANIFEST_H
#define COSMOMANIFEST_H

#include <stdint.h>

#define COSMO_MANIFEST_MAGIC   "COSMOMAN"   /* 8 bytes, no NUL */
#define COSMO_MANIFEST_VERSION 1u
#define COSMO_MANIFEST_FILES   3u
#define COSMO_MANIFEST_NAME_MAX 16u

/* The files, by index. The name field holds exactly these, NUL padded. */
#define COSMO_MANIFEST_KERNEL  0u   /* "kernel.elf" */
#define COSMO_MANIFEST_ARCHIVE 1u   /* "boot.tar" */
#define COSMO_MANIFEST_CMDLINE 2u   /* "cmdline" */

struct cosmo_manifest_file {
    char     name[COSMO_MANIFEST_NAME_MAX];
    uint64_t size;
    uint8_t  sha512[64];
};

struct cosmo_manifest {
    char     magic[8];
    uint32_t version;
    uint32_t nfiles;          /* COSMO_MANIFEST_FILES */
    struct cosmo_manifest_file files[COSMO_MANIFEST_FILES];
};

/* The signature trailer, as modsign.py writes it. */
#define COSMO_MANIFEST_SIG_MAGIC   "COSMOSIG"
#define COSMO_MANIFEST_SIG_VERSION 1u
#define COSMO_MANIFEST_SIG_ED25519 1u

struct cosmo_manifest_sig {
    uint8_t  sig[64];
    uint8_t  key_id[8];
    uint32_t version;
    uint32_t algo;
    char     magic[8];
};

#define COSMO_MANIFEST_FILE_SIZE (sizeof(struct cosmo_manifest) + sizeof(struct cosmo_manifest_sig))

_Static_assert(sizeof(struct cosmo_manifest_file) == 88, "manifest file entry layout");
_Static_assert(sizeof(struct cosmo_manifest) == 280, "manifest payload layout");
_Static_assert(sizeof(struct cosmo_manifest_sig) == 88, "signature trailer layout");

#endif /* COSMOMANIFEST_H */
