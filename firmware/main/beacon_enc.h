/* Re-encodes the stand-alone beacon loop on the device (front panel: service name, provider, pilots).
 * LDPC is systematic, so the BBFRAMEs can be read back out of the built-in loop (beacon.bin, flash): the encoder takes them from
 * there, replaces the SDT packet of the first frame, and runs BB scrambling -> BCH -> LDPC -> QPSK -> PLHEADER / pilots -> PL scrambling
 * again, bit-exact with host/dvbs2.py. No video encoding: the test card keeps the text it was made with.
 * Plain C without ESP-IDF calls, so the same file builds on the PC for the bit-exact test (host/test_beacon_enc.py). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BENC_NAME_MAX 20                                         /* service name / provider, characters */

uint32_t benc_frame_syms(bool pilots);                           /* symbols of one PLFRAME */
uint32_t benc_frames(void);                                      /* PLFRAMEs in the built-in loop */
size_t benc_loop_bytes(bool pilots);                             /* loop buffer size, including the 16 bytes past its end */
/* Builds the loop into dst (benc_loop_bytes(pilots) bytes) from the built-in loop src. Returns the loop length in symbols, 0 when the
 * built-in loop does not look as expected (no SDT in packet 3) or out of memory (2 KB of parity bits). */
uint32_t benc_build(uint8_t *dst, const uint8_t *src, bool pilots, const char *service, const char *provider);
