#pragma once

#include <cstdint>
#include <vector>

namespace kumo::scene {

struct EntityId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const EntityId&, const EntityId&) = default;
};

// Generational slot map: ids stay valid until removed; a stale id (whose slot was
// reused) resolves to null. Minimal vector-of-slots plus a free list.
template <typename T> class SlotMap {
public:
    EntityId insert(const T& value) {
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& slot = slots_[index];
        slot.occupied = true;
        slot.value = value;
        ++live_;
        return EntityId{index, slot.generation};
    }

    bool remove(EntityId id) {
        Slot* slot = slotOf(id);
        if (slot == nullptr) {
            return false;
        }
        slot->occupied = false;
        ++slot->generation;
        slot->value = T{};
        free_.push_back(id.index);
        --live_;
        return true;
    }

    // Preserves slot generations so ids handed out before the clear keep missing.
    void clear() {
        free_.clear();
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].occupied) {
                slots_[i].occupied = false;
                ++slots_[i].generation;
                slots_[i].value = T{};
            }
            free_.push_back(i);
        }
        live_ = 0;
    }

    T* get(EntityId id) {
        Slot* slot = slotOf(id);
        return slot != nullptr ? &slot->value : nullptr;
    }

    const T* get(EntityId id) const {
        const Slot* slot = slotOf(id);
        return slot != nullptr ? &slot->value : nullptr;
    }

    std::size_t size() const { return live_; }
    bool empty() const { return live_ == 0; }

    template <typename Fn> void forEach(Fn&& fn) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].occupied) {
                fn(EntityId{i, slots_[i].generation}, slots_[i].value);
            }
        }
    }

    template <typename Fn> void forEach(Fn&& fn) const {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].occupied) {
                fn(EntityId{i, slots_[i].generation}, slots_[i].value);
            }
        }
    }

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool occupied = false;
        T value{};
    };

    Slot* slotOf(EntityId id) {
        if (id.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[id.index];
        if (!slot.occupied || slot.generation != id.generation) {
            return nullptr;
        }
        return &slot;
    }

    const Slot* slotOf(EntityId id) const {
        if (id.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[id.index];
        if (!slot.occupied || slot.generation != id.generation) {
            return nullptr;
        }
        return &slot;
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_ = 0;
};

} // namespace kumo::scene
