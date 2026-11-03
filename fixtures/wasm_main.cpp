// One fixture as a WASM bot: built once per fixture, with UD_BOT_HEADER and UD_BOT_CLASS set
// by fixtures/CMakeLists.txt. The fixture's own source is unchanged; see sdk/cpp/include/ud/bot_host.hpp.
#include UD_BOT_HEADER
#include <ud/bot_host.hpp>

UD_WASM_BOT(UD_BOT_CLASS)
