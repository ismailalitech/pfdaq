/* Real 32-bit MMIO accesses for the Python tools (Python's mmap/ctypes stores go through
 * memcpy, which on this target writes byte by byte and corrupts APB register writes). */
typedef unsigned int uint32_t;
uint32_t mmio_rd32(const volatile uint32_t *p) { return *p; }
void mmio_wr32(volatile uint32_t *p, uint32_t v) { *p = v; }
