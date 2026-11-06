#pragma once
#include "adaptive_bot.hpp"

namespace game {

// AdaptiveBot biased towards attack: its first Factory comes at once, it expands with one
// claimer, and it pushes with any army at least even with what it has seen (or a small one
// when it has seen none), strikes before it has fully gathered and fights on longer.
class AggroBot : public AdaptiveBot {
public:
  AggroBot()
    : AdaptiveBot({.max_claimers = 1,
                   .claims_before_factory = 0,
                   .push_margin_pct = 100,
                   .min_push_value = 80,
                   .blind_push_value = 120,
                   .retreat_pct = 35,
                   .gather_pct = 60}) {}
};

} // namespace game
