// The runner offers a bot no imports, but wasi-libc (which a bot using the C++ standard library
// links, through wasi-sdk) reaches the host for its startup, stdio and exit paths. Defining
// its import thunks here resolves them inside the module. None of them is reached in a
// working bot; one that is reached (an abort, say) traps, and a trap forfeits the tick.
#include <cstdint>

#define UD_WASI_TRAP(name, ...)                                                             \
  extern "C" int32_t __imported_wasi_snapshot_preview1_##name(__VA_ARGS__) {                \
    __builtin_trap();                                                                       \
  }

UD_WASI_TRAP(args_get, int32_t, int32_t)
UD_WASI_TRAP(args_sizes_get, int32_t, int32_t)
UD_WASI_TRAP(environ_get, int32_t, int32_t)
UD_WASI_TRAP(environ_sizes_get, int32_t, int32_t)
UD_WASI_TRAP(fd_close, int32_t)
UD_WASI_TRAP(fd_fdstat_get, int32_t, int32_t)
UD_WASI_TRAP(fd_seek, int32_t, int64_t, int32_t, int32_t)
UD_WASI_TRAP(fd_write, int32_t, int32_t, int32_t, int32_t)
UD_WASI_TRAP(fd_read, int32_t, int32_t, int32_t, int32_t)

extern "C" [[noreturn]] void __imported_wasi_snapshot_preview1_proc_exit(int32_t) {
  __builtin_trap();
}
