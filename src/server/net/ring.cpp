#include "ring.hpp"
#include "../http/exchange.hpp"

#include <exception>
#include <sys/eventfd.h>
#include <utility>

namespace tds::net
{

Ring::Ring(unsigned slots, bool primary) : connections(slots), wake_operation{Event::wake, *this}
{
    exchange_pool_.reserve(max_pooled_exchanges);
    if (!primary)
    {
        notification = std::make_unique<Descriptor>(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    }
    const int result = io_uring_queue_init(1024, &queue, 0);
    if (result < 0)
    {
        throw std::system_error(-result, std::generic_category(), "io_uring setup");
    }
    try
    {
        const int registered = io_uring_register_files_sparse(&queue, slots);
        if (registered < 0)
        {
            throw std::system_error(-registered, std::generic_category(), "fixed file table");
        }
        receive_buffers = std::make_unique<ReceiveBuffers>(queue);
        if (notification)
        {
            const int notified = io_uring_register_eventfd(&queue, notification->get());
            if (notified < 0)
            {
                throw std::system_error(-notified, std::generic_category(), "ring eventfd");
            }
        }
    }
    catch (...)
    {
        receive_buffers.reset();
        io_uring_queue_exit(&queue);
        throw;
    }
}

Ring::~Ring()
{
    // Fatal exits stop kernel access before either connection or send storage is freed
    if (pending)
    {
        io_uring_sync_cancel_reg request{};
        request.flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
        request.timeout.tv_sec = 5;
        const int result = io_uring_register_sync_cancel(&queue, &request);
        if (result < 0 && result != -ENOENT)
        {
            std::terminate();
        }
    }
    receive_buffers.reset();
    io_uring_queue_exit(&queue);
}

std::unique_ptr<http::Exchange> Ring::acquire_exchange()
{
    if (exchange_pool_.empty())
    {
        return std::make_unique<http::Exchange>();
    }
    auto exchange = std::move(exchange_pool_.back());
    exchange_pool_.pop_back();

    return exchange;
}

void Ring::recycle_exchange(std::unique_ptr<http::Exchange> exchange)
{
    if (exchange_pool_.size() == max_pooled_exchanges ||
        exchange->retained_capacity() > max_retained_capacity)
    {
        return;
    }
    exchange_pool_.push_back(std::move(exchange));
}

} // namespace tds::net
