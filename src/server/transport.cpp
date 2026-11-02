#include "transport.hpp"
#include "net/reactor.hpp"
#include "transport/connection.hpp"

#ifdef TDS_DIAGNOSTICS
#include "transport/diagnostics.hpp"
#endif

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tds
{
namespace
{
using net::Completion;
using net::CompletionKind;
using net::Connection;
using net::Reactor;
} // namespace

class Server::State
{
  public:
    State(std::string_view host, std::uint16_t port, Handler handle, HeaderHandler head,
          BatchHandler write_handler)
        : reactor(host, port), handler(std::move(handle)), headers(std::move(head)),
          writes(std::move(write_handler))
    {
    }

    void request_stop() const noexcept
    {
        reactor.request_stop();
    }

    [[nodiscard]] int stop_descriptor() const noexcept
    {
        return reactor.stop_descriptor();
    }

    [[nodiscard]] std::uint16_t port() const noexcept
    {
        return reactor.port();
    }

    void run()
    {
#ifdef TDS_DIAGNOSTICS
        diagnostics.start();

        struct FinalReport
        {
            State &owner;

            ~FinalReport()
            {
                owner.diagnostics.report(owner.reactor.rings(), true);
            }
        } report_on_exit{*this};
#endif
        reactor.start();
        while (reactor.pending_count())
        {
            reactor.begin_turn();
            Completion completion;
            while (reactor.next(completion))
            {
                complete(completion);
                if (pending_writes.commands.size() == write_batch_limit)
                {
                    flush_writes();
                }
            }
            // Flush before the reactor waits so a partial batch cannot strand its replies
            flush_writes();
            reactor.finish_turn();
#ifdef TDS_DIAGNOSTICS
            diagnostics.report(reactor.rings(), false);
#endif
        }
    }

  private:
    RequestResult handle(Connection &connection)
    {
#ifdef TDS_DIAGNOSTICS
        const auto &request = connection.exchange->session.request();
        if (request.endpoint == Endpoint::create || request.endpoint == Endpoint::like)
        {
            return handler(request);
        }
        diagnostics.receive_to_handler_ns.record(timing::now_ns() -
                                                 connection.exchange->receive_serviced_ns);
        timing::Scope sample(diagnostics.handler_wall_ns);

        return handler(request);
#else
        return handler(connection.exchange->session.request());
#endif
    }

    void send(Connection &connection)
    {
#ifdef TDS_DIAGNOSTICS
        connection.exchange->send_prepared_ns = timing::now_ns();
#endif
        auto &message = connection.exchange->reply.message();
        reactor.send(connection, message);
    }

    void respond(Connection &connection, Response response)
    {
        connection.close_after =
            connection.close_after || !connection.exchange->session.request().keep_alive;
        connection.exchange->reply.prepare(std::move(response), connection.close_after);
        send(connection);
    }

    void flush_writes()
    {
        if (pending_writes.commands.empty())
        {
            return;
        }
        const auto count = pending_writes.commands.size();
        const auto connections = std::span(pending_writes.connections).first(count);
        if (reactor.stopping())
        {
            for (auto *connection : connections)
            {
                reactor.close(*connection);
            }
            pending_writes.commands.clear();

            return;
        }
        const auto responses = std::span(pending_writes.responses).first(count);
        try
        {
#ifdef TDS_DIAGNOSTICS
            diagnostics.write_batch_size.record(count);
            timing::Scope sample(diagnostics.write_batch_wall_ns);
#endif
            writes(pending_writes.commands, responses);
        }
        catch (const std::exception &error)
        {
            std::cerr << "write batch failed: " << error.what() << '\n';
            for (auto &response : responses)
            {
                response = {Status::internal_error, "{\"error\":\"internal server error\"}"};
            }
        }
        // Queued commands and their targets stay alive until replies have been staged
        for (std::size_t index = 0; index < count; ++index)
        {
            auto &connection = *connections[index];
            connection.close_after =
                connection.close_after || responses[index].status == Status::internal_error;
            respond(connection, std::move(responses[index]));
        }
        pending_writes.commands.clear();
    }

    void queue_write(Connection &connection, WriteCommand command)
    {
        const auto index = pending_writes.commands.size();
        pending_writes.commands.push_back(std::move(command));
        pending_writes.connections[index] = &connection;
        connection.start_queued_write();
    }

    void process(Connection &connection)
    {
        auto &exchange = *connection.exchange;
        auto &session = exchange.session;
        try
        {
            while (true)
            {
                const auto result = session.consume();
                if (!result.valid)
                {
                    connection.close_after = true;
                    respond(connection,
                            {session.error(), "{\"error\":\"malformed HTTP request\"}"});

                    return;
                }
                if (session.headers_pending())
                {
                    if (auto rejected = headers(session.request()))
                    {
                        // Unread body bytes must not become the next keep-alive request
                        connection.close_after = true;
                        respond(connection, std::move(*rejected));

                        return;
                    }
                    session.accept_headers();
                    continue;
                }
                if (session.complete())
                {
                    auto outcome = handle(connection);
                    if (auto *response = std::get_if<Response>(&outcome))
                    {
                        respond(connection, std::move(*response));
                    }
                    else
                    {
                        queue_write(connection, std::get<WriteCommand>(std::move(outcome)));
                    }
                }
                else
                {
                    reactor.receive(connection);
                }

                return;
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "request failed: " << error.what() << '\n';
            connection.close_after = true;
            respond(connection, {Status::internal_error, "{\"error\":\"internal server error\"}"});
        }
    }

    void complete_receive(Completion &completion)
    {
        auto &connection = *completion.connection;
#ifdef TDS_DIAGNOSTICS
        const auto received_ns = timing::now_ns();
#endif
        if (!connection.exchange)
        {
            connection.exchange = connection.ring.acquire_exchange();
        }
#ifdef TDS_DIAGNOSTICS
        // A buffered pipeline request keeps this time through earlier response sends
        connection.exchange->receive_serviced_ns = received_ns;
#endif
        const auto input = completion.buffer.bytes();
        connection.exchange->session.append({input.data(), input.size()});
        completion.buffer.release();
        process(connection);
    }

    void complete_send(const Completion &completion)
    {
        auto &connection = *completion.connection;
        const auto result = completion.result;
#ifdef TDS_DIAGNOSTICS
        connection.ring.diagnostics.send_attempt.record(completion.serviced_ns -
                                                        connection.exchange->send_prepared_ns);
#endif
        // Fatal and shutdown completions have already started closing in the reactor
        if (connection.io_closing())
        {
            return;
        }
        if (result < 0)
        {
            send(connection);

            return;
        }
        auto &exchange = *connection.exchange;
        if (!exchange.reply.advance(static_cast<std::size_t>(result)))
        {
            send(connection);

            return;
        }
        if (connection.close_after)
        {
            reactor.close(connection);

            return;
        }
        exchange.reply.reset();
        exchange.session.reset();
        if (exchange.session.has_input())
        {
            process(connection);

            return;
        }
        connection.ring.recycle_exchange(std::move(connection.exchange));
        reactor.receive(connection);
    }

    void complete(Completion &completion)
    {
        switch (completion.kind)
        {
        case CompletionKind::control:
            return;
        case CompletionKind::received:
            complete_receive(completion);
            return;
        case CompletionKind::sent:
            complete_send(completion);
            return;
        }
    }

    Reactor reactor;
    Handler handler;
    HeaderHandler headers;
    BatchHandler writes;

    struct PendingWrites
    {
        PendingWrites()
        {
            commands.reserve(write_batch_limit);
        }

        std::array<Connection *, write_batch_limit> connections{};
        std::array<Response, write_batch_limit> responses{};
        std::vector<WriteCommand> commands;
    } pending_writes;
#ifdef TDS_DIAGNOSTICS
    transport::TransportDiagnostics diagnostics;
#endif
};

Server::Server(std::string_view host, std::uint16_t port, Handler handler, HeaderHandler headers,
               BatchHandler writes)
    : state_(std::make_unique<State>(host, port, std::move(handler), std::move(headers),
                                     std::move(writes)))
{
}

Server::~Server() = default;

void Server::run()
{
    state_->run();
}

void Server::request_stop() const noexcept
{
    state_->request_stop();
}

int Server::stop_descriptor() const noexcept
{
    return state_->stop_descriptor();
}

std::uint16_t Server::port() const noexcept
{
    return state_->port();
}

} // namespace tds
