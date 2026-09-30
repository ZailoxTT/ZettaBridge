/* Compresses and decompresses with the guest's own libz. Android's zlib runs its inflate loop on
 * NEON, and a single mistranslated vector operation shows up as a corrupt stream rather than as
 * an error: Unity reported "Inflate Error: invalid stored block lengths" and closed. */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

enum { kSize = 256 * 1024 };
static unsigned char source[kSize];
static unsigned char packed[kSize * 2];
static unsigned char restored[kSize];

int main(void) {
    /* Compressible but not trivial, so inflate really uses its window and its chunked copies. */
    for (int i = 0; i < kSize; ++i) {
        source[i] = (unsigned char)((i * 7 + (i >> 5) * 31 + (i % 97)) & 0xFF);
        if ((i % 64) < 20) source[i] = (unsigned char)(i >> 8);
    }
    uLongf packed_size = sizeof packed;
    if (compress2(packed, &packed_size, source, kSize, 6) != Z_OK) {
        printf("compress failed\n");
        return 1;
    }
    uLongf restored_size = sizeof restored;
    const int rc = uncompress(restored, &restored_size, packed, packed_size);
    if (rc != Z_OK) {
        printf("uncompress failed: %d\n", rc);
        return 1;
    }
    printf("packed %lu bytes, restored %lu, identical: %d\n", (unsigned long)packed_size,
           (unsigned long)restored_size, memcmp(source, restored, kSize) == 0);
    printf("crc32 %08lx adler32 %08lx\n", (unsigned long)crc32(0, source, kSize),
           (unsigned long)adler32(1, source, kSize));
    return 0;
}
