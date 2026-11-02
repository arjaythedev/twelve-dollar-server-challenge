#include "ring.hpp"

#include <exception>
#include <sys/eventfd.h>

namespace tds::transport
{

ReceiveBuffers::ReceiveBuffers(io_uring &owner)
    : ring(owner), storage(std::make_unique_for_overwrite<char[]>(count * size))
{
    int error = 0;
    buffers = io_uring_setup_buf_ring(&ring, count, group, 0, &error);
    if (!buffers)
    {
        throw std::system_error(-error, std::generic_category(), "receive buffer ring");
    }
    for (unsigned id = 0; id < count; ++id)
    {
        io_uring_buf_ring_add(buffers, data(id), size, static_cast<unsigned short>(id),
                              io_uring_buf_ring_mask(count), static_cast<int>(id));
    }
    io_uring_buf_ring_advance(buffers, count);
}

ReceiveBuffers::~ReceiveBuffers()
{
    if (io_uring_free_buf_ring(&ring, buffers, count, group) < 0)
    {
        std::terminate();
    }
}

Ring::Ring(unsigned slots, bool primary)
    : connections(slots), accept_operation{Event::accept, *this}, wake_operation{Event::wake, *this}
{
    free_slots.reserve(slots);
    for (unsigned index = slots; index > 0; --index)
    {
        free_slots.push_back(index - 1);
    }
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
    // Fatal exits must stop kernel access before any connection storage is freed.
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

} // namespace tds::transport
