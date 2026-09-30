/* File reading as an engine does it: mmap of a whole file, mmap of a window at an offset, pread
 * at odd offsets, and a short read at the end. Unity failed to inflate data out of its own APK
 * while the guest's zlib was provably fine, so what it read is what had to be checked. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { kPage = 4096, kPages = 64, kSize = kPage * kPages };

static unsigned char byte_at(long i) {
    return (unsigned char)((i * 131 + (i >> 7) * 17 + (i % 251)) & 0xFF);
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/data/local/tmp/zb_filemap.tmp";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("open failed\n"); return 1; }
    unsigned char* source = malloc(kSize);
    for (long i = 0; i < kSize; ++i) source[i] = byte_at(i);
    if (write(fd, source, kSize) != kSize) { printf("write failed\n"); return 1; }

    int whole_ok = 1;
    unsigned char* whole = mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (whole == MAP_FAILED) { printf("mmap whole failed\n"); return 1; }
    for (long i = 0; i < kSize; ++i) if (whole[i] != source[i]) { whole_ok = 0; break; }

    /* A window in the middle, the shape a zip reader uses for one entry. */
    const long window_offset = kPage * 7;
    const long window_size = kPage * 5;
    int window_ok = 1;
    unsigned char* window = mmap(NULL, window_size, PROT_READ, MAP_PRIVATE, fd, window_offset);
    if (window == MAP_FAILED) { printf("mmap window failed\n"); return 1; }
    for (long i = 0; i < window_size; ++i) {
        if (window[i] != source[window_offset + i]) { window_ok = 0; break; }
    }

    /* pread at offsets that are not page multiples, and a read that runs past the end. */
    int pread_ok = 1;
    unsigned char buffer[1000];
    for (long offset = 1; offset + (long)sizeof buffer <= kSize; offset += 4093) {
        if (pread(fd, buffer, sizeof buffer, offset) != (ssize_t)sizeof buffer) { pread_ok = 0; break; }
        if (memcmp(buffer, source + offset, sizeof buffer) != 0) { pread_ok = 0; break; }
    }
    const ssize_t tail = pread(fd, buffer, sizeof buffer, kSize - 10);

    /* lseek to a large offset, as a reader of a big archive does. */
    const off_t sought = lseek(fd, kSize - 100, SEEK_SET);
    const ssize_t after_seek = read(fd, buffer, sizeof buffer);

    printf("whole %d window %d pread %d tail %d seek %d read %d\n", whole_ok, window_ok, pread_ok,
           (int)tail, (int)(sought == kSize - 100), (int)after_seek);
    munmap(whole, kSize);
    munmap(window, window_size);
    close(fd);
    unlink(path);
    return 0;
}
