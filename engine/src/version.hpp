#pragma once
#include "config.hpp"
#include <cstdio>
#include <cstring>

namespace game {

// The build's version (cmake/Version.cmake): git describe of the source tree, or -DUD_VERSION.
const char* build_version();

// Every tool takes --version: prints "<tool> <version> (ABI <n>)" and returns true, for main to
// exit 0. The ABI is there because it, not the version, says which bots and replays fit.
inline bool print_version_if_asked(int argc, char** argv, const char* tool) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--version")) {
      std::printf("%s %s (ABI %u)\n", tool, build_version(), cfg::ABI_VERSION);
      return true;
    }
  return false;
}

} // namespace game
