# validate_bot.py's bot build: one bot.cpp, built the way every ladder bot is, by
# ai_game_add_bot() from an installed engine. Configured by the script:
#   cmake -S prompts/build -B <dir> -DCMAKE_PREFIX_PATH=<engine install> -DBOT=<bot.cpp>
# and gives <dir>/bot.wasm.
cmake_minimum_required(VERSION 3.20)
project(oneshot_bot LANGUAGES CXX)

if(NOT BOT)
  message(FATAL_ERROR "set -DBOT=<path to bot.cpp>")
endif()

find_package(ai_game REQUIRED)
ai_game_add_bot(bot SOURCES ${BOT})
