#pragma once
#include "adaptive_bot.hpp"

namespace game {

// AdaptiveBot biased towards expanding and holding ground: four claimers out at once, the
// first Factory only once three claims stand, a wider guard around its structures, and a push
// only with an army well beyond anything it has seen; it falls back at the first setback.
class BoomBot : public AdaptiveBot {
public:
  BoomBot()
    : AdaptiveBot({.max_claimers = 4,
                   .claims_before_factory = 3,
                   .defend_radius = 12,
                   .push_margin_pct = 250,
                   .min_push_value = 400,
                   .blind_push_value = 500,
                   .retreat_pct = 85}) {}
};

} // namespace game
