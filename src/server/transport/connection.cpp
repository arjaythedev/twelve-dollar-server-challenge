#include "connection.hpp"
#include "../net/connection.hpp"

namespace tds::net
{

Connection::Connection(Ring &owner, DirectSlot index)
    : ring(owner), operation{Event::receive, *this}, close_operation{Event::close, *this},
      slot(index)
{
}

Connection::~Connection() = default;

} // namespace tds::net
