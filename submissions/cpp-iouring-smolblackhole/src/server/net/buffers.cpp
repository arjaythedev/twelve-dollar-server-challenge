#include "buffers.hpp"

#include <exception>
#include <system_error>

namespace tds::net
{

ReceiveBuffers::ReceiveBuffers(io_uring &owner)
    : ring_(owner), storage_(std::make_unique_for_overwrite<char[]>(count * size))
{
    int error = 0;
    buffers_ = io_uring_setup_buf_ring(&ring_, count, group, 0, &error);
    if (!buffers_)
    {
        throw std::system_error(-error, std::generic_category(), "receive buffer ring");
    }
    for (unsigned id = 0; id < count; ++id)
    {
        io_uring_buf_ring_add(buffers_, data(id), size, static_cast<unsigned short>(id),
                              io_uring_buf_ring_mask(count), static_cast<int>(id));
    }
    io_uring_buf_ring_advance(buffers_, count);
}

ReceiveBuffers::~ReceiveBuffers()
{
    if (io_uring_free_buf_ring(&ring_, buffers_, count, group) < 0)
    {
        std::terminate();
    }
}

} // namespace tds::net
