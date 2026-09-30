/* The pre-ARMv7 memory barriers, written through CP15 instead of DMB/DSB/ISB. Mono emits them in
 * its own barrier, and Thomas Was Alone died on the first one: the translator ended the process
 * outright rather than raising anything catchable. */
#include <stdio.h>
int main(void) {
    int value = 0;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" :: "r"(0) : "memory");  /* DMB */
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");  /* DSB */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(0) : "memory");   /* ISB */
    value = 7;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" :: "r"(0) : "memory");
    printf("cp15 barriers survived: %d\n", value);
    return 0;
}
