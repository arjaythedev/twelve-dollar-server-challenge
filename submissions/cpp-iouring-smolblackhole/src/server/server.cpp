#include "server.hpp"
#include "app/application.hpp"
#include "diagnostics/io.hpp"
#include "http/exchange.hpp"
#include "net/reactor.hpp"

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
    State(std::string_view host, std::uint16_t port, std::string database_path, std::string secret)
        : application(std::move(database_path), std::move(secret)), reactor(host, port)
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
        const auto report_on_exit = telemetry.run(reactor);
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
            telemetry.report(reactor, false);
        }
    }

  private:
    void dispatch(Connection &connection)
    {
        const auto &session = connection.exchange->session;
        auto sample = telemetry.handler(connection);
        auto prepared = application.prepare(session.accepted_head(), session.message().body);
        if (!prepared)
        {
            sample.finish();
            connection.close_after =
                connection.close_after || prepared.error().status == Status::internal_error;
            respond(connection, Application::error_response(prepared.error()));

            return;
        }
        if (auto *write = std::get_if<WriteCommand>(&*prepared))
        {
            queue_write(connection, std::move(*write));

            return;
        }
        auto response = application.read(std::get<ReadQuery>(*prepared),
                                         connection.exchange->reply.take_body());
        sample.finish();
        respond(connection, std::move(response));
    }

    void send(Connection &connection)
    {
        telemetry.send_prepared(connection);
        auto &message = connection.exchange->reply.message();
        reactor.send(connection, message);
    }

    void respond(Connection &connection, Response response)
    {
        connection.close_after =
            connection.close_after || !connection.exchange->session.message().keep_alive;
        connection.exchange->reply.prepare(std::move(response), connection.close_after);
        send(connection);
    }

    void flush_writes()
    {
        if (pending_writes.commands.empty() || pending_writes.resolved)
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
            pending_writes.clear();

            return;
        }
        const auto responses = std::span(pending_writes.responses).first(count);
        const auto outcomes = std::span(pending_writes.outcomes).first(count);
        try
        {
            const auto sample = telemetry.write_batch(count);
            const auto committed = application.commit(pending_writes.commands, outcomes);
            // Serialization or send failures must never replay already committed commands
            pending_writes.resolved = true;
            if (!committed)
            {
                throw std::runtime_error(committed.error().message);
            }
            for (std::size_t index = 0; index < count; ++index)
            {
                try
                {
                    responses[index] =
                        application.write_reply(pending_writes.commands[index], outcomes[index],
                                                connections[index]->exchange->reply.take_body());
                }
                catch (const std::exception &error)
                {
                    std::cerr << "request failed: " << error.what() << '\n';
                    responses[index] = Application::error_response(
                        {Status::internal_error, "internal server error"});
                }
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "write batch failed: " << error.what() << '\n';
            pending_writes.resolved = true;
            for (auto &response : responses)
            {
                response =
                    Application::error_response({Status::internal_error, "internal server error"});
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
        pending_writes.clear();
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
                    respond(connection, Application::error_response(
                                            {session.error(), "malformed HTTP request"}));

                    return;
                }
                if (session.headers_pending())
                {
                    const auto &message = session.message();
                    auto accepted = application.accept_headers(
                        {message.method, message.path, message.authorization});
                    if (!accepted)
                    {
                        // Unread body bytes must not become the next keep-alive request
                        connection.close_after = true;
                        respond(connection, Application::error_response(accepted.error()));

                        return;
                    }
                    session.accept_headers(std::move(*accepted));
                    continue;
                }
                if (session.complete())
                {
                    dispatch(connection);
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
            respond(connection,
                    Application::error_response({Status::internal_error, "internal server error"}));
        }
    }

    void complete_receive(Completion &completion)
    {
        const auto received = telemetry.receive();
        auto &connection = *completion.connection;
        if (!connection.exchange)
        {
            connection.exchange = connection.ring.acquire_exchange();
        }
        received.attach(connection);
        const auto input = completion.buffer.bytes();
        connection.exchange->session.append({input.data(), input.size()});
        completion.buffer.release();
        process(connection);
    }

    void complete_send(const Completion &completion)
    {
        auto &connection = *completion.connection;
        const auto result = completion.result;
        telemetry.send_completed(completion);
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

    // Keep the application alive until reactor-owned I/O storage has retired
    Application application;
    Reactor reactor;

    struct WriteBatch
    {
        WriteBatch()
        {
            commands.reserve(write_batch_limit);
        }

        void clear() noexcept
        {
            // Keep the slots, but release previous results and error strings after publishing
            for (auto &outcome : std::span(outcomes).first(commands.size()))
            {
                outcome.emplace();
            }
            commands.clear();
            resolved = false;
        }

        std::array<Connection *, write_batch_limit> connections{};
        std::array<Response, write_batch_limit> responses{};
        std::array<WriteOutcome, write_batch_limit> outcomes{};
        std::vector<WriteCommand> commands;
        bool resolved = false;
    } pending_writes;

    [[no_unique_address]] telemetry::IoTelemetry telemetry;
};

Server::Server(std::string_view host, std::uint16_t port, std::string database_path,
               std::string secret)
    : state_(std::make_unique<State>(host, port, std::move(database_path), std::move(secret)))
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
