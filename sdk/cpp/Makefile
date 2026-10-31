CLANG   ?= clang++-18
TARGET   = wasm32-unknown-unknown

# Engine ABI headers live in engine/src; SDK helpers live in include/.
CXXFLAGS = --target=$(TARGET)        \
           -I../../engine/src        \
           -Iinclude                 \
           -nostdlib -fno-exceptions -fno-rtti \
           -O2 -ffp-contract=off -fno-fast-math \
           -mno-bulk-memory          \
           -Wall

# The runner rejects bulk-memory instructions, so large copies/clears compile to calls into
# runtime.cpp's memcpy/memset/memmove instead (the build is -nostdlib; nothing else provides them).
RUNTIME  = runtime.cpp

# wasm-ld exports data symbols by address, so SNAPSHOT_ADDR and COMMAND_ADDR
# become WASM i32 globals pointing at their linear-memory locations.
LDFLAGS  = -Wl,--no-entry            \
           -Wl,--export=abi_version  \
           -Wl,--export=init         \
           -Wl,--export=on_tick      \
           -Wl,--export=SNAPSHOT_ADDR \
           -Wl,--export=COMMAND_ADDR \
           -Wl,--export-memory

# Default: compile bot_template.cpp.  Override: make BOT=my_bot.cpp
BOT     ?= bot_template.cpp
OUT      = $(patsubst %.cpp,%.wasm,$(BOT))

.PHONY: all examples clean

all: $(OUT)

$(OUT): $(BOT) $(RUNTIME) $(HEADERS)
	$(CLANG) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(RUNTIME)

examples: examples/idle_bot.wasm examples/rush_bot.wasm \
          examples/quadratic_bot.wasm examples/leaky_bot.wasm \
          examples/copy_bot.wasm

HEADERS = include/ud/types.hpp ../../engine/src/abi.hpp ../../engine/src/config.hpp

examples/%.wasm: examples/%.cpp $(RUNTIME) $(HEADERS)
	$(CLANG) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(RUNTIME)

clean:
	rm -f *.wasm examples/*.wasm
