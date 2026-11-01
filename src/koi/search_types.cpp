#include "koi/search_types.hpp"

#include "koi/game_state.hpp"

namespace koi {

SearchRequestIdentity SearchRequestIdentity::from(const GameState& root,
                                                  const std::uint64_t generation) {
    return SearchRequestIdentity{generation, root.position_key(), root.fen()};
}

} // namespace koi
