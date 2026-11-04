#include "../load/inputs.hpp"
#include "../load/runtime.hpp"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace loadgen;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error("self-test: " + std::string(message));
    }
}

enum class FixtureMode
{
    workflow,
    failed_feed,
    stalled,
    partially_stalled,
    closed
};

class Fixture
{
  public:
    explicit Fixture(FixtureMode mode)
        : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), mode_(mode)
    {
        require(listener_.get() >= 0, "fixture socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener_.get(), reinterpret_cast<const sockaddr *>(&address),
                       sizeof(address)) == 0,
                "fixture bind");
        require(::listen(listener_.get(), 16) == 0, "fixture listen");
        socklen_t size = sizeof(address);
        require(::getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&address), &size) == 0,
                "fixture address");
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop) {
            try
            {
                serve(stop);
            }
            catch (...)
            {
                error_ = std::current_exception();
            }
        });
    }

    ~Fixture()
    {
        worker_.request_stop();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    [[nodiscard]] Endpoint target() const
    {
        return endpoint("http://127.0.0.1:" + std::to_string(port_));
    }

    void check()
    {
        worker_.request_stop();
        worker_.join();
        if (error_)
        {
            std::rethrow_exception(error_);
        }
    }

    [[nodiscard]] unsigned connections() const
    {
        return connections_;
    }

  private:
    struct Client
    {
        Descriptor socket;
        std::string input;
        std::optional<std::size_t> token;
        std::string selected;
        unsigned stage = 0;
        bool silent = false;
    };

    void send_all(int fd, std::string_view response)
    {
        while (!response.empty())
        {
            const auto sent = ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR)
            {
                continue;
            }
            if (sent <= 0)
            {
                return;
            }
            response.remove_prefix(static_cast<std::size_t>(sent));
        }
    }

    void respond(Client &client, std::string_view request)
    {
        const auto first_space = request.find(' ');
        const auto second_space = request.find(' ', first_space + 1);
        const auto path = request.substr(first_space + 1, second_space - first_space - 1);
        if (mode_ == FixtureMode::stalled ||
            (mode_ == FixtureMode::partially_stalled && client.silent))
        {
            return;
        }
        if (path == "/feed")
        {
            require(client.stage == 0 || client.stage >= 2, "workflow feed order");
            client.stage = 1;
            if (mode_ == FixtureMode::failed_feed)
            {
                client.stage = 0;
                send_all(client.socket.get(),
                         "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n");
            }
            else if (mode_ == FixtureMode::closed)
            {
                send_all(client.socket.get(), "HTTP/1.1 200 OK\r\nConnection: "
                                              "close\r\n\r\n{\"posts\":[{\"id\":7},{\"id\":9}]}");
                client.socket.reset();
            }
            else
            {
                // Chunked framing and an informational response exercise the live socket path
                send_all(client.socket.get(), "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 "
                                              "OK\r\nTransfer-Encoding: chunked\r\n\r\n");
                send_all(client.socket.get(),
                         "1d\r\n{\"posts\":[{\"id\":7},{\"id\":9}]}\r\n0\r\n\r\n");
            }
            return;
        }
        if (path == "/posts/7" || path == "/posts/9")
        {
            require(client.stage == 1 || mode_ == FixtureMode::closed, "workflow post order");
            client.selected = path;
            client.stage = 2;
            send_all(client.socket.get(), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
            return;
        }
        std::optional<std::size_t> token;
        for (std::size_t index = 0; index < 3; ++index)
        {
            if (request.find("Authorization: Bearer token" + std::to_string(index) + "\r\n") !=
                std::string_view::npos)
            {
                token = index;
            }
        }
        require(token.has_value(), "POST authorization");
        require(!client.token || client.token == token, "one VU token per persistent socket");
        client.token = token;
        require(request.find("Content-Type: application/json\r\n") != std::string_view::npos,
                "POST content type");
        if (path == "/posts/7/like" || path == "/posts/9/like")
        {
            require(client.stage == 2 && path == client.selected + "/like", "workflow like order");
            client.stage = 3;
        }
        else
        {
            require(path == "/posts" && (client.stage == 2 || client.stage == 3),
                    "workflow create order");
            client.stage = 4;
            const auto body_start = request.find("\r\n\r\n") + 4;
            const auto document = json(request.substr(body_start));
            require(document != nullptr, "create JSON");
            auto *body = yyjson_obj_get(yyjson_doc_get_root(document.get()), "body");
            require(yyjson_is_str(body), "create body string");
            const std::string_view text(yyjson_get_str(body), yyjson_get_len(body));
            require(text.starts_with("user" + std::to_string(*token) + " says hi at "),
                    "create username mapping");
            const auto vu_start = text.find(" (VU ");
            require(vu_start != std::string_view::npos && text.ends_with(')'),
                    "create VU/iteration suffix");
            const auto vu_end = text.find(',', vu_start);
            const auto vu =
                integer(text.substr(vu_start + 5, vu_end - vu_start - 5), 100, "fixture VU");
            require((vu - 1) % 3 == *token, "VU token modulo mapping");
        }
        send_all(client.socket.get(), "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\n{}");
    }

    void serve(std::stop_token stop)
    {
        std::vector<Client> clients;
        while (!stop.stop_requested())
        {
            std::vector<pollfd> descriptors{{listener_.get(), POLLIN, 0}};
            for (const auto &client : clients)
            {
                descriptors.push_back({client.socket.get(), POLLIN, 0});
            }
            const int ready = ::poll(descriptors.data(), descriptors.size(), 10);
            if (ready < 0 && errno == EINTR)
            {
                continue;
            }
            require(ready >= 0, "fixture poll");
            for (std::size_t index = 0; index < clients.size(); ++index)
            {
                if (descriptors[index + 1].revents == 0)
                {
                    continue;
                }
                auto &client = clients[index];
                std::array<char, 4096> buffer{};
                const auto received = ::recv(client.socket.get(), buffer.data(), buffer.size(), 0);
                if (received <= 0)
                {
                    client.socket.reset();
                    continue;
                }
                client.input.append(buffer.data(), static_cast<std::size_t>(received));
                const auto header_end = client.input.find("\r\n\r\n");
                if (header_end == std::string::npos)
                {
                    continue;
                }
                const auto length_header = client.input.find("\r\nContent-Length: ");
                std::size_t content_length = 0;
                if (length_header != std::string::npos)
                {
                    const auto length_start = length_header + 18;
                    const auto length_end = client.input.find("\r\n", length_start);
                    const std::string_view value(client.input.data() + length_start,
                                                 length_end - length_start);
                    const auto [end, error] =
                        std::from_chars(value.data(), value.data() + value.size(), content_length);
                    require(error == std::errc{} && end == value.data() + value.size(),
                            "fixture content length");
                }
                if (client.input.size() < header_end + 4 + content_length)
                {
                    continue;
                }
                require(client.input.size() == header_end + 4 + content_length,
                        "no pipelined VU requests");
                respond(client, client.input);
                client.input.clear();
            }
            std::erase_if(clients, [](const Client &client) {
                return client.socket.get() < 0;
            });
            if ((descriptors[0].revents & POLLIN) != 0)
            {
                const int socket = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
                require(socket >= 0, "fixture accept");
                const int enabled = 1;
                ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
                Client client;
                client.socket.reset(socket);
                client.silent = connections_ % 2 != 0;
                clients.push_back(std::move(client));
                ++connections_;
            }
        }
    }

    Descriptor listener_;
    FixtureMode mode_;
    unsigned short port_ = 0;
    unsigned connections_ = 0;
    std::exception_ptr error_;
    std::jthread worker_;
};

Metrics fixture_run(Fixture &fixture, RuntimeConfig config)
{
    std::vector<Token> tokens;
    for (unsigned index = 0; index < 3; ++index)
    {
        tokens.push_back({"user" + std::to_string(index),
                          "Authorization: Bearer token" + std::to_string(index) + "\r\n"});
    }
    Load load(config, fixture.target(), std::move(tokens), "127.0.0.2-127.0.0.5");
    auto metrics = load.run(false);
    fixture.check();
    metrics.latencies.prepare();
    return metrics;
}

void self_test()
{
    require(duration("1m500ms") == 60.5, "compound duration");
    require(random_seed("0") == 0 &&
                random_seed("18446744073709551615") == std::numeric_limits<std::uint64_t>::max(),
            "fixed RNG seed accepts the full unsigned 64-bit range");
    for (const auto invalid : {"", "-1", "42x", "18446744073709551616"})
    {
        bool rejected = false;
        try
        {
            static_cast<void>(random_seed(invalid));
        }
        catch (const std::runtime_error &)
        {
            rejected = true;
        }
        require(rejected, "invalid fixed RNG seed is rejected");
    }
    require(local_addresses("127.0.0.2-127.0.0.4,127.0.0.9").size() == 4, "local IPv4 ranges");
    require(create_body({"alice", ""}, 3, 7, "2026-11-02T12:00:00.123Z") ==
                "{\"body\":\"alice says hi at 2026-11-02T12:00:00.123Z (VU 3, iter 7)\"}",
            "create body");
    std::mt19937_64 random(42);
    require(pick_post("{\"posts\":[{\"id\":1},{\"id\":2}]}", random).has_value(), "feed JSON");
    require(!pick_post("{\"posts\":[]}", random), "empty feed");
    const std::array<std::string, 3> responses = {
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\nTrailer: "
        "yes\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"};
    for (const auto &response : responses)
    {
        for (std::size_t split = 0; split <= response.size(); ++split)
        {
            Request request(Step::feed);
            require(request.receive(std::string_view(response).substr(0, split)),
                    "first HTTP fragment");
            if (split < response.size())
            {
                require(request.receive(std::string_view(response).substr(split)),
                        "second HTTP fragment");
            }
            require(request.complete && request.body == "abc" && request.keep_alive,
                    "framed response");
        }
    }
    Request eof(Step::feed);
    require(eof.receive("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc") && !eof.complete,
            "EOF framing waits");
    require(eof.eof() && eof.body == "abc" && !eof.keep_alive, "EOF response completion");
    Request truncated(Step::feed);
    require(truncated.receive("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc") &&
                !truncated.eof(),
            "truncated response rejected");
    Request extra(Step::feed);
    require(!extra.receive(responses[0] + "unexpected"), "unsolicited response bytes rejected");
    std::vector<User> users(10);
    Timers timers(users);
    for (std::size_t id = 0; id < users.size(); ++id)
    {
        users[id].timer = static_cast<double>(10 - id);
        timers.update(id);
    }
    for (std::size_t id = users.size(); id > 0; --id)
    {
        require(timers.first() == id - 1, "indexed timer order");
        timers.remove(id - 1);
    }
    require(timers.empty(), "timer removal");
    const std::vector<double> samples{0, 1, 2, 3};
    require(percentile(samples, 0.5) == 1.5 && std::abs(percentile(samples, 0.95) - 2.85) < 1e-12,
            "linear quantiles");
    DiagnosticHistogram histogram;
    require(histogram.quantile(0.99).upper == 0 && histogram.count == 0,
            "empty diagnostic histogram");
    const std::vector<double> diagnostic_samples{0,     0.0001,   0.0005, 0.001, 0.001001, 0.015999,
                                                 0.016, 0.016001, 1,      500,   1000,     60000};
    for (const double sample : diagnostic_samples)
    {
        histogram.record(sample);
        DiagnosticHistogram single;
        single.record(sample);
        const auto bounds = single.quantile(0.99);
        require(bounds.lower <= sample && sample <= bounds.upper && single.maximum_ms == sample,
                "diagnostic single-sample bounds include sub-microsecond input");
    }
    for (const double fraction : {0.0, 0.5, 0.95, 0.99, 1.0})
    {
        const auto bounds = histogram.quantile(fraction);
        const double exact = percentile(diagnostic_samples, fraction);
        require(bounds.lower <= exact && exact <= bounds.upper,
                "diagnostic quantile bounds contain exact interpolation");
    }
    std::uint64_t histogram_count = 0;
    for (const auto count : histogram.bins)
    {
        histogram_count += count;
    }
    require(histogram_count == diagnostic_samples.size() && histogram.maximum_ms == 60000,
            "diagnostic histogram count and exact maximum");
    auto diagnostics = std::make_unique<Diagnostics>();
    require(sizeof(Diagnostics) < 4 * 1024 * 1024, "diagnostic fixed storage stays below 4 MiB");
    diagnostics->request(4.999, 1);
    diagnostics->request(5, 2);
    diagnostics->timer(5.001, 5);
    diagnostics->epoll(5.002, 5);
    diagnostics->timer_batch(5, 5.003, 7);
    diagnostics->repoll(5.005, 5.001);
    require(diagnostics->retained(0).request_duration.count == 1 &&
                diagnostics->retained(1).request_duration.count == 1 &&
                std::abs(diagnostics->retained(1).timer_overdue.maximum_ms - 1) < 1e-9 &&
                std::abs(diagnostics->retained(1).epoll_service_lag.maximum_ms - 2) < 1e-9 &&
                diagnostics->retained(1).timer_batches == 1 &&
                diagnostics->retained(1).timer_batch_max_services == 7 &&
                std::abs(diagnostics->retained(1).timer_batch_max_ms - 3) < 1e-9 &&
                diagnostics->retained(1).epoll_polls == 1 &&
                std::abs(diagnostics->retained(1).epoll_repoll_max_ms - 4) < 1e-9,
            "diagnostic window boundaries and service delays");
    diagnostics->timer(640, 639.75);
    diagnostics->request(4.5, 3);
    diagnostics->timer(640.001, 641);
    diagnostics->request(645, 4);
    require(diagnostics->retained(128).sequence == 128 &&
                diagnostics->retained(128).timer_overdue.count == 2 &&
                diagnostics->retained(128).timer_overdue.maximum_ms == 250 &&
                diagnostics->retained(128).timer_overdue.bins[0] == 1 &&
                diagnostics->overwritten_windows == 2 && diagnostics->dropped_late_samples == 1,
            "diagnostic ring wrap preserves current timers and rejects expired samples");
    diagnostics->workload_start_ns = 987654321;
    std::ostringstream diagnostic_output;
    diagnostic_summary(diagnostic_output, *diagnostics, 646);
    const auto diagnostic_document = json(diagnostic_output.str());
    auto *diagnostic_root =
        diagnostic_document ? yyjson_doc_get_root(diagnostic_document.get()) : nullptr;
    auto *diagnostic_windows = yyjson_obj_get(diagnostic_root, "windows");
    auto *diagnostic_clock = yyjson_obj_get(diagnostic_root, "clock");
    require(
        yyjson_is_str(diagnostic_clock) &&
            std::string_view(yyjson_get_str(diagnostic_clock)).find("local") !=
                std::string_view::npos &&
            std::string_view(yyjson_get_str(diagnostic_clock)).find("same epoch as server") ==
                std::string_view::npos &&
            yyjson_get_uint(yyjson_obj_get(diagnostic_root, "workload_start_ns")) == 987654321 &&
            yyjson_get_uint(yyjson_obj_get(diagnostic_root, "windows_retained")) == 2 &&
            yyjson_arr_size(diagnostic_windows) == 2 &&
            yyjson_get_num(yyjson_obj_get(yyjson_arr_get(diagnostic_windows, 0), "start_s")) ==
                640 &&
            yyjson_get_num(yyjson_obj_get(yyjson_arr_get(diagnostic_windows, 1), "end_s")) == 646,
        "diagnostic summary retains its local time origin and bounded windows");
    RuntimeConfig phases;
    phases.ramp_up = 2;
    phases.hold = 3;
    phases.ramp_down = 4;
    require(completion_phase(phases, 0) == Phase::ramp_up &&
                completion_phase(phases, 1.999) == Phase::ramp_up &&
                completion_phase(phases, 2) == Phase::hold &&
                completion_phase(phases, 4.999) == Phase::hold &&
                completion_phase(phases, 5) == Phase::ramp_down &&
                completion_phase(phases, 8.999) == Phase::ramp_down &&
                completion_phase(phases, 9) == Phase::grace,
            "completion phase boundaries");
    const auto grouped_count = [](const std::array<LatencyStatistics, 4> &groups) {
        std::size_t count = 0;
        for (const auto &group : groups)
        {
            count += group.count;
        }
        return count;
    };
    RuntimeConfig config;
    config.vus = 4;
    config.ramp_up = 0.005;
    config.hold = 0.5;
    config.ramp_down = 0.005;
    config.grace = 0.04;
    config.timeout = 0.1;
    config.time_scale = 0.0001;
    config.seed = 42;
    config.diagnostics = true;
    Fixture workflow(FixtureMode::workflow);
    auto metrics = fixture_run(workflow, config);
    require(metrics.diagnostics && metrics.diagnostics->requests == metrics.latencies.size() &&
                metrics.diagnostics->workload_start_ns > 0 && metrics.diagnostics->timers > 0 &&
                metrics.diagnostics->events > 0,
            "live diagnostic counts and time origin");
    require(grouped_count(metrics.latencies.summary().endpoints) == metrics.latencies.size() &&
                grouped_count(metrics.latencies.summary().phases) == metrics.latencies.size(),
            "latency group counts equal global count");
    for (std::size_t index = 0; index < metrics.latencies.summary().endpoints.size(); ++index)
    {
        require(metrics.latencies.summary().endpoints[index].count ==
                    metrics.endpoint_requests[index],
                "latency endpoint classification");
    }
    require(metrics.failures == 0 && metrics.workflow_errors == 0, "live workflow HTTP");
    require(metrics.endpoint_requests[0] > 0 && metrics.endpoint_requests[1] > 0 &&
                metrics.endpoint_requests[2] > 0 && metrics.endpoint_requests[3] > 0,
            "all workflow branches");
    require(workflow.connections() == config.vus && metrics.connections == config.vus,
            "independent persistent VU sockets");
    require(metrics.established_connections == config.vus &&
                metrics.live_connections == config.vus && metrics.closed_connections == 0 &&
                metrics.peak_live_connections == config.vus &&
                metrics.hold_peak_live_connections == config.vus &&
                metrics.vus_with_http_response == config.vus,
            "healthy coverage includes successful handshakes and retained idle sockets");
    require(metrics.target_activation_seconds && summarize(metrics, config).passed,
            "healthy real workload activates all VUs on time and reaches connection coverage");
    Metrics late_activation;
    late_activation.latencies.record(1, EndpointKind::feed, Phase::hold);
    late_activation.hold_peak_live_connections = config.vus;
    late_activation.vus_with_http_response = config.vus;
    late_activation.target_activation_seconds = config.ramp_up + 0.100;
    require(summarize(late_activation, config).passed,
            "activation deadline includes the explicit 100ms scheduler margin");
    late_activation.target_activation_seconds = config.ramp_up + 0.101;
    const auto late_summary = summarize(late_activation, config);
    const auto late_document = json(late_summary.json);
    require(late_document != nullptr, "late activation summary JSON");
    auto *late_generator = yyjson_obj_get(yyjson_doc_get_root(late_document.get()), "generator");
    require(
        !late_summary.passed &&
            yyjson_get_bool(yyjson_obj_get(late_generator, "http_metrics_passed")) &&
            yyjson_get_bool(yyjson_obj_get(late_generator, "target_concurrency_reached")) &&
            yyjson_get_bool(yyjson_obj_get(late_generator, "http_response_coverage_complete")) &&
            !yyjson_get_bool(yyjson_obj_get(late_generator, "target_activation_on_time")) &&
            yyjson_get_uint(yyjson_obj_get(late_generator, "activation_deadline_grace_ms")) ==
                100 &&
            std::abs(yyjson_get_num(yyjson_obj_get(late_generator, "target_activation_seconds")) -
                     *late_activation.target_activation_seconds) < 1e-12,
        "late activation cannot pass despite successful HTTP, connection peak and coverage");
    late_activation.target_activation_seconds.reset();
    const auto missing_activation = summarize(late_activation, config);
    const auto missing_document = json(missing_activation.json);
    require(missing_document != nullptr, "missing activation summary JSON");
    auto *missing_generator =
        yyjson_obj_get(yyjson_doc_get_root(missing_document.get()), "generator");
    require(!missing_activation.passed &&
                yyjson_is_null(yyjson_obj_get(missing_generator, "target_activation_seconds")) &&
                !yyjson_get_bool(yyjson_obj_get(missing_generator, "target_activation_on_time")),
            "missing activation time is explicit and cannot pass");
    require(metrics.interrupted_iterations == 0 && metrics.iterations > 0,
            "retirement finishes iteration");
    RuntimeConfig burst = config;
    burst.vus = 1024;
    burst.ramp_up = 0;
    burst.hold = 0.04;
    burst.ramp_down = 0;
    burst.grace = 0.2;
    burst.timeout = 0.2;
    Fixture activation_burst(FixtureMode::failed_feed);
    auto bounded = fixture_run(activation_burst, burst);
    require(bounded.diagnostics && bounded.diagnostics->timers >= burst.vus &&
                bounded.diagnostics->events > 0 && !bounded.latencies.empty(),
            "overdue activation burst retains timers and services real socket events");
    const auto &burst_window = bounded.diagnostics->retained(0);
    require(burst_window.timer_batches > 1 && burst_window.epoll_polls > 1 &&
                burst_window.timer_batch_max_services > 0 &&
                burst_window.timer_batch_max_services <= max_timer_batch,
            "activation timers yield to epoll before exceeding the per-pass limit");
    const auto bounded_metrics = summarize(bounded, burst);
    const auto bounded_document = json(bounded_metrics.json);
    require(bounded_document != nullptr, "bounded activation summary JSON");
    auto *bounded_diagnostics = yyjson_obj_get(
        yyjson_obj_get(yyjson_doc_get_root(bounded_document.get()), "generator"), "diagnostics");
    auto *bounded_window = yyjson_arr_get(yyjson_obj_get(bounded_diagnostics, "windows"), 0);
    require(yyjson_get_uint(yyjson_obj_get(bounded_window, "timer_batch_max_services")) ==
                    burst_window.timer_batch_max_services &&
                yyjson_get_uint(yyjson_obj_get(bounded_window, "epoll_polls")) ==
                    burst_window.epoll_polls &&
                yyjson_is_num(yyjson_obj_get(bounded_window, "epoll_repoll_max_ms")),
            "summary exports timer-budget and pre-poll service gaps");
    Fixture failed_feed(FixtureMode::failed_feed);
    config.hold = 0.04;
    const auto failed = fixture_run(failed_feed, config);
    require(failed.diagnostics && failed.diagnostics->requests == failed.latencies.size(),
            "failed requests included in diagnostic latency");
    require(grouped_count(failed.latencies.summary().endpoints) == failed.latencies.size() &&
                grouped_count(failed.latencies.summary().phases) == failed.latencies.size(),
            "failed samples retained in latency groups");
    require(failed.failures > 0 && failed.endpoint_requests[1] == 0 &&
                failed.endpoint_requests[2] == 0 && failed.endpoint_requests[3] == 0,
            "failed feed ends iteration");
    require(failed.vus_with_http_response == config.vus,
            "HTTP 503 responses count toward VU coverage without passing HTTP thresholds");
    RuntimeConfig partial_config = config;
    partial_config.vus = 2;
    partial_config.ramp_up = 0;
    partial_config.ramp_down = 0;
    partial_config.hold = 0.04;
    partial_config.grace = 0.01;
    Fixture partial(FixtureMode::partially_stalled);
    auto incomplete = fixture_run(partial, partial_config);
    require(incomplete.failures == 0 && !incomplete.latencies.empty() &&
                incomplete.established_connections == partial_config.vus &&
                incomplete.hold_peak_live_connections == partial_config.vus &&
                incomplete.vus_with_http_response == 1 && incomplete.interrupted_iterations > 0,
            "partially silent VUs leave zero HTTP failures but incomplete response coverage");
    require(!summarize(incomplete, partial_config).passed,
            "partially silent workload cannot pass using only its completed HTTP responses");
    Fixture closed(FixtureMode::closed);
    config.vus = 1;
    config.hold = 0.04;
    const auto reconnected = fixture_run(closed, config);
    require(reconnected.failures == 0 && reconnected.connections > 1 &&
                reconnected.endpoint_requests[1] > 0,
            "EOF response reconnects");
    require(reconnected.established_connections == reconnected.connections &&
                reconnected.closed_connections + reconnected.live_connections ==
                    reconnected.established_connections &&
                reconnected.live_connections <= 1 && reconnected.peak_live_connections == 1 &&
                reconnected.vus_with_http_response == 1,
            "reconnections count each handshake and close without duplicating VU coverage");
    Fixture stalled(FixtureMode::stalled);
    config.hold = 0.01;
    config.ramp_up = 0;
    config.ramp_down = 0;
    config.grace = 0.005;
    auto canceled = fixture_run(stalled, config);
    require(canceled.diagnostics && canceled.diagnostics->requests == 0,
            "canceled requests omitted from diagnostic latency");
    require(grouped_count(canceled.latencies.summary().endpoints) == 0 &&
                grouped_count(canceled.latencies.summary().phases) == 0,
            "canceled samples omitted from latency groups");
    require(canceled.interrupted_iterations == 1 && canceled.latencies.empty() &&
                canceled.failures == 0 && canceled.iterations == 0,
            "grace cancellation omits in-flight request");
    require(canceled.established_connections == 1 && canceled.closed_connections == 1 &&
                canceled.live_connections == 0 && canceled.vus_with_http_response == 0 &&
                !summarize(canceled, config).passed,
            "silent cancellation closes the established socket and cannot pass an empty run");
    Fixture timeout(FixtureMode::stalled);
    config.hold = 0.025;
    config.timeout = 0.005;
    const auto timed_out = fixture_run(timeout, config);
    require(timed_out.diagnostics && timed_out.diagnostics->requests == timed_out.latencies.size(),
            "timed out requests included in diagnostic latency");
    require(timed_out.failures > 0 && timed_out.checks_failed[0] > 0 &&
                timed_out.latencies.summary().overall.minimum >= 5,
            "request timeout is a real failed sample");
    std::cout << "Load parser, timer, payload, quantile, socket and workflow tests passed\n";
}
} // namespace

int main()
{
    try
    {
        self_test();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
