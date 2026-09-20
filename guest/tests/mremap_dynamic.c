// mremap used to fail with ENOMEM for every call, so a guest allocator that grows a mapping had
// no way forward. The guest reservation is ours, so a move is a new mapping plus a copy.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void fill(char* p, size_t len, char seed) {
    for (size_t i = 0; i < len; ++i) p[i] = (char)(seed + (char)(i % 61));
}

static int check(const char* p, size_t len, char seed) {
    for (size_t i = 0; i < len; ++i) {
        if (p[i] != (char)(seed + (char)(i % 61))) return 0;
    }
    return 1;
}

int main(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);

    char* one = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (one == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    fill(one, page, 'a');

    /* Growing: the mapping may stay or move, but the bytes must come along. */
    char* grown = mremap(one, page, 4 * page, MREMAP_MAYMOVE);
    if (grown == MAP_FAILED) { printf("grow failed: %s\n", strerror(errno)); return 1; }
    printf("grow kept contents: %d\n", check(grown, page, 'a'));
    fill(grown + page, 3 * page, 'b');
    printf("grown tail writable: %d\n", check(grown + page, 3 * page, 'b'));

    /* Shrinking keeps the address and the head of the data. */
    char* shrunk = mremap(grown, 4 * page, 2 * page, 0);
    if (shrunk == MAP_FAILED) { printf("shrink failed: %s\n", strerror(errno)); return 1; }
    printf("shrink kept address: %d\n", shrunk == grown);
    printf("shrink kept contents: %d\n", check(shrunk, page, 'a'));

    /* Without MREMAP_MAYMOVE a blocked neighbour has to fail rather than move the mapping. */
    char* neighbour = mmap(shrunk + 2 * page, page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (neighbour == MAP_FAILED) { printf("neighbour mmap failed\n"); return 1; }
    char* stuck = mremap(shrunk, 2 * page, 8 * page, 0);
    printf("blocked grow refused: %d\n", stuck == MAP_FAILED && errno == ENOMEM);

    /* Read-only protection survives a move. */
    char* ro = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) { printf("ro mmap failed\n"); return 1; }
    fill(ro, page, 'c');
    if (mprotect(ro, page, PROT_READ) != 0) { printf("mprotect failed\n"); return 1; }
    char* moved = mremap(ro, page, 16 * page, MREMAP_MAYMOVE);
    if (moved == MAP_FAILED) { printf("ro move failed: %s\n", strerror(errno)); return 1; }
    printf("read-only move kept contents: %d\n", check(moved, page, 'c'));

    munmap(shrunk, 2 * page);
    munmap(neighbour, page);
    munmap(moved, 16 * page);
    return 0;
}
