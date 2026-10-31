// Freestanding memcpy/memset/memmove, linked into every SDK bot.
//
// Bots are built with -mno-bulk-memory (the runner rejects memory.copy/memory.fill, which cost
// one unit of fuel regardless of size), so the compiler lowers large copies and clears to calls
// to these. no_builtin stops clang recognising the loops below as memcpy/memset and compiling
// them back into calls to themselves.
#include <stddef.h>
#include <stdint.h>

extern "C" {

__attribute__((no_builtin)) void* memcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  const auto* s = static_cast<const unsigned char*>(src);
  if (((reinterpret_cast<uintptr_t>(d) | reinterpret_cast<uintptr_t>(s)) & 3) == 0) {
    for (; n >= 4; n -= 4, d += 4, s += 4)
      *reinterpret_cast<uint32_t*>(d) = *reinterpret_cast<const uint32_t*>(s);
  }
  for (; n > 0; --n) *d++ = *s++;
  return dst;
}

__attribute__((no_builtin)) void* memset(void* dst, int c, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  const auto b = static_cast<unsigned char>(c);
  if ((reinterpret_cast<uintptr_t>(d) & 3) == 0) {
    const uint32_t word = b * 0x01010101u;
    for (; n >= 4; n -= 4, d += 4) *reinterpret_cast<uint32_t*>(d) = word;
  }
  for (; n > 0; --n) *d++ = b;
  return dst;
}

__attribute__((no_builtin)) void* memmove(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  const auto* s = static_cast<const unsigned char*>(src);
  if (d <= s || d >= s + n) return memcpy(dst, src, n);
  while (n > 0) {
    --n;
    d[n] = s[n];
  }
  return dst;
}

} // extern "C"
