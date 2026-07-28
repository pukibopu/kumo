#include <kumo/facade/undo_stack.h>

#include <utility>

namespace kumo::facade {

UndoStack::UndoStack(Capture capture, Apply apply, std::size_t depth)
    : capture_(std::move(capture)), apply_(std::move(apply)), depth_(depth) {}

void UndoStack::beginPending(std::string label) {
    pending_ = Entry{std::move(label), capture_()};
}

void UndoStack::commitPending() {
    if (!pending_.has_value()) {
        return;
    }
    undo_.push_back(std::move(*pending_));
    pending_.reset();
    if (undo_.size() > depth_) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

void UndoStack::discardPending() {
    pending_.reset();
}

bool UndoStack::hasPending() const {
    return pending_.has_value();
}

bool UndoStack::canUndo() const {
    return !undo_.empty();
}

bool UndoStack::canRedo() const {
    return !redo_.empty();
}

const std::string* UndoStack::undoLabel() const {
    return undo_.empty() ? nullptr : &undo_.back().label;
}

const std::string* UndoStack::redoLabel() const {
    return redo_.empty() ? nullptr : &redo_.back().label;
}

bool UndoStack::undo() {
    discardPending();
    if (undo_.empty()) {
        return false;
    }
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({entry.label, capture_()});
    apply_(entry.state);
    return true;
}

bool UndoStack::redo() {
    discardPending();
    if (redo_.empty()) {
        return false;
    }
    Entry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({entry.label, capture_()});
    apply_(entry.state);
    return true;
}

} // namespace kumo::facade
