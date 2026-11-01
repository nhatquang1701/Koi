#pragma once

// Stateless FEN syntax validation used by GameState::from_fen before the
// native position and the compatibility mirror are built.  Kept separate from
// the facade implementation so the syntax rules can be unit-tested and audited
// without linking the whole game-state translation unit.

#include <string_view>

namespace koi::detail {

[[nodiscard]] bool valid_fen_syntax(std::string_view fen);

} // namespace koi::detail
