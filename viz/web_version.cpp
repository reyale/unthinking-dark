// The browser build's version: Makefile.em passes git describe, as cmake/Version.cmake does.
#include "version.hpp"

const char* game::build_version() { return UD_WEB_VERSION; }
