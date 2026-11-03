#include "connections.hpp"
#include "../diagnostics/io.hpp"
#include "../http/exchange.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace tds::net
{

Connection::Connection(Ring &owner, DirectSlot index)
    : ring(owner), operation{Event::receive, *this}, close_operation{Event::shutdown, *this},
      slot(index)
{
}

Connection::~Connection() = default;

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
    telemetry::IoTelemetry::store_created(*this);

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
    telemetry::IoTelemetry::store_retired(*this);
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

#if TDS_TELEMETRY_LEVEL >= 1
std::size_t ConnectionStore::live_count() const noexcept
{
    return live_count_;
}

std::size_t ConnectionStore::reserved_count() const noexcept
{
    return records_.size() - free_slots_.size() - live_count_;
}
#endif

} // namespace tds::net
