#include "koi/detail/evaluation_context.hpp"

namespace koi::detail {

EvaluationContext::EvaluationContext(const Evaluator& evaluator,
                                     std::mutex* evaluator_mutex)
    : evaluator_(evaluator), evaluator_mutex_(evaluator_mutex),
      worker_(evaluator.create_worker()) {}

EvaluationContext::~EvaluationContext() = default;

int EvaluationContext::evaluate(const GameState& state, const Color perspective) {
    if (worker_) {
        return worker_->evaluate(state, perspective);
    }
    if (evaluator_mutex_ != nullptr) {
        std::lock_guard lock(*evaluator_mutex_);
        return evaluator_.evaluate(state, perspective);
    }
    return evaluator_.evaluate(state, perspective);
}

int EvaluationContext::evaluate_with_key(const GameState& state, const Color perspective,
                                         const Color side_to_move,
                                         const std::uint64_t position_key) {
    if (worker_) {
        return worker_->evaluate_with_key(state, perspective, side_to_move, position_key);
    }
    if (evaluator_mutex_ != nullptr) {
        std::lock_guard lock(*evaluator_mutex_);
        return evaluator_.evaluate(state, perspective);
    }
    return evaluator_.evaluate(state, perspective);
}

} // namespace koi::detail
