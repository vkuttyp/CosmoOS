/*
 * lz4.h - LZ4 block compression
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 6").
 *
 * The LZ4 block format: a sequence of tokens, each a literal run and a
 * match copied from earlier output. No framing, no checksum, no
 * dictionary -- the caller knows the sizes and checks the data, which
 * here is the filesystem's business and not the codec's.
 *
 * Pure: no allocation, no locks, no globals. Compiled on the host by
 * tests/host/test_lz4.c and fuzzed by tests/fuzz/fuzz_lz4.c, which is
 * where the evidence for the decompressor's bounds comes from.
 */

#ifndef KERNEL_LZ4_H
#define KERNEL_LZ4_H

#include <kernel/types.h>

/* The most `len` bytes can turn into: incompressible input costs a byte
 * of token per 255 literals. */
static inline size_t lz4_compress_bound(size_t len)
{
    return len + len / 255u + 16u;
}

/* Compress `slen` bytes into at most `dcap`. Returns the compressed
 * length, or 0 when it does not fit -- which is not an error: the caller
 * stores the block as it is. */
size_t lz4_compress(const void *src, size_t slen, void *dst, size_t dcap);

/* Decompress into at most `dcap` bytes. Returns the decompressed length,
 * or 0 for input that is malformed or would write past the end. Every
 * offset and length in the stream is checked against both buffers: this
 * runs on bytes that came off a disk.
 *
 * Decoding stops when the output is full *or* the input runs out at a
 * sequence boundary. A caller that knows the decompressed length passes
 * it as `dcap` and gets it back; whatever follows the stream in the
 * input -- the padding to the end of a disk block, say -- is never
 * looked at. */
size_t lz4_decompress(const void *src, size_t slen, void *dst, size_t dcap);

#endif /* KERNEL_LZ4_H */
