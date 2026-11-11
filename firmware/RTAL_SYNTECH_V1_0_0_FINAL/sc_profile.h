/* FIX4: low-overhead sampled CPU-cycle profiling. Only audio core writes. */
#ifndef SC_PROFILE_H
#define SC_PROFILE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { volatile uint32_t seq, voices, control, osc1, osc2, osc2_core, osc2_noise, mixfilter, general, general_voices, cross2_voices, chorus, reverb, master; } sc_profile_t;
extern sc_profile_t sc_prof;
extern volatile uint8_t sc_prof_active;
static inline uint32_t sc_prof_tick(void) {
#if defined(__XTENSA__)
    uint32_t c; __asm__ __volatile__("rsr.ccount %0" : "=a"(c)); return c;
#else
    return 0;
#endif
}
#ifdef __cplusplus
}
#endif
#endif
