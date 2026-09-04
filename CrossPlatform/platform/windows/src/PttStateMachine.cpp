#include "localflow/windows/PttStateMachine.hpp"

namespace localflow::windows {

PttTransition PttStateMachine::press(const Trigger trigger) noexcept {
    if (active_.has_value()) {
        return PttTransition::none;
    }
    active_ = trigger;
    return PttTransition::pressed;
}

PttTransition PttStateMachine::release(const Trigger trigger) noexcept {
    if (!active_.has_value() || *active_ != trigger) {
        return PttTransition::none;
    }
    active_.reset();
    return PttTransition::released;
}

PttTransition PttStateMachine::cancel() noexcept {
    if (!active_.has_value()) {
        return PttTransition::none;
    }
    active_.reset();
    return PttTransition::cancelled;
}

void PttStateMachine::reset() noexcept {
    active_.reset();
}

}  // namespace localflow::windows
