#include "connections.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace tds::net
{

ConnectionStore::ConnectionStore(unsigned slots) : records_(slots)
{
    free_slots_.reserve(slots);
    for (unsigned index = slots; index > 0; --index)
    {
        free_slots_.push_back(DirectSlot{index - 1});
    }
}

bool ConnectionStore::has_free_slot() const noexcept
{
    return !free_slots_.empty();
}

DirectSlot ConnectionStore::reserve()
{
    if (free_slots_.empty())
    {
        throw std::logic_error("no free direct descriptor slot");
    }
    const auto slot = free_slots_.back();
    free_slots_.pop_back();

    return slot;
}

void ConnectionStore::release(DirectSlot slot)
{
    assert(slot.value < records_.size() && !records_[slot.value]);
    free_slots_.push_back(slot);
}

Connection &ConnectionStore::create(Ring &owner, DirectSlot slot)
{
    assert(slot.value < records_.size() && !records_[slot.value]);
    auto connection = std::make_unique<Connection>(owner, slot);
    auto &record = *connection;
    records_[slot.value] = std::move(connection);

    return record;
}

void ConnectionStore::retire(DirectSlot slot)
{
    auto &record = records_.at(slot.value);
    if (!record || !record->io_closing())
    {
        throw std::logic_error("retiring a connection before its close completion");
    }
    record.reset();
    free_slots_.push_back(slot);
}

Connection *ConnectionStore::at(std::size_t index) const noexcept
{
    assert(index < records_.size());

    return records_[index].get();
}

std::size_t ConnectionStore::size() const noexcept
{
    return records_.size();
}

} // namespace tds::net
