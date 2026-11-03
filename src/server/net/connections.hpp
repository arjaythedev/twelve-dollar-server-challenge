#pragma once

#include "connection.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace tds::net
{

class ConnectionStore
{
  public:
    explicit ConnectionStore(unsigned slots);
    ConnectionStore(const ConnectionStore &) = delete;
    ConnectionStore &operator=(const ConnectionStore &) = delete;

    bool has_free_slot() const noexcept;
    DirectSlot reserve();
    void release(DirectSlot slot);
    Connection &create(Ring &owner, DirectSlot slot);
    void retire(DirectSlot slot);
    Connection *at(std::size_t index) const noexcept;
    std::size_t size() const noexcept;
#ifdef TDS_DIAGNOSTICS
    // Live records include closing connections until their terminal close CQE
    std::size_t live_count() const noexcept;
    std::size_t reserved_count() const noexcept;
#endif

  private:
    std::vector<std::unique_ptr<Connection>> records_;
    std::vector<DirectSlot> free_slots_;
#ifdef TDS_DIAGNOSTICS
    std::size_t live_count_ = 0;
#endif
};

} // namespace tds::net
