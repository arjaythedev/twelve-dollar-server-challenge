#include "runtime.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netinet/tcp.h>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>

namespace loadgen
{
namespace
{
void append_integer(std::string &output, std::uint64_t value)
{
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits;
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    output.append(digits.data(), static_cast<std::size_t>(result.ptr - digits.data()));
}
} // namespace

volatile std::sig_atomic_t interrupted = 0;

void stop_signal(int)
{
    interrupted = 1;
}

std::optional<std::uint64_t> pick_post(std::string_view body, std::mt19937_64 &random)
{
    const auto document = json(body);
    auto *root = document ? yyjson_doc_get_root(document.get()) : nullptr;
    auto *posts = yyjson_obj_get(root, "posts");
    const auto size = yyjson_arr_size(posts);
    if (size == 0)
    {
        return std::nullopt;
    }
    const auto position = std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    auto *id = yyjson_obj_get(yyjson_arr_get(posts, position), "id");
    if (yyjson_is_uint(id))
    {
        return yyjson_get_uint(id);
    }
    return std::nullopt;
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const auto value = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    gmtime_r(&value, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
           << millis << 'Z';
    return output.str();
}

std::string create_body(const Token &token, std::size_t vu, std::uint64_t iteration,
                        std::string_view when)
{
    return "{\"body\":" +
           json_string(token.username + " says hi at " + std::string(when) + " (VU " +
                       std::to_string(vu) + ", iter " + std::to_string(iteration) + ")") +
           '}';
}

Phase completion_phase(const RuntimeConfig &config, double completed)
{
    if (completed < config.ramp_up)
    {
        return Phase::ramp_up;
    }
    if (completed < config.ramp_up + config.hold)
    {
        return Phase::hold;
    }
    if (completed < config.ramp_up + config.hold + config.ramp_down)
    {
        return Phase::ramp_down;
    }
    return Phase::grace;
}

Load::Load(RuntimeConfig config, Endpoint target, std::vector<Token> tokens,
           std::string_view local_ips)
    : config_(std::move(config)), target_(std::move(target)), tokens_(std::move(tokens)),
      local_ips_(local_addresses(local_ips)), random_(config_.seed),
      epoll_(::epoll_create1(EPOLL_CLOEXEC)), users_(config_.vus), timers_(users_.size())
{
    request_headers_ =
        " HTTP/1.1\r\nHost: " + target_.authority +
        "\r\nUser-Agent: native-load/0.1.0\r\nAccept: */*\r\nConnection: keep-alive\r\n";
    feed_request_ = "GET " + target_.prefix + "/feed" + request_headers_ + "\r\n";
    get_post_prefix_ = "GET " + target_.prefix + "/posts/";
    write_post_prefix_ = "POST " + target_.prefix + "/posts";
    if (epoll_.get() < 0)
    {
        throw std::runtime_error("epoll_create1: " + std::string(std::strerror(errno)));
    }
    if (config_.diagnostics)
    {
        metrics_.diagnostics = std::make_unique<Diagnostics>();
    }
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || config_.vus + 16 > limit.rlim_cur)
    {
        throw std::runtime_error("file descriptor limit is too low; set ulimit -n above VUS + 16");
    }
    for (std::size_t id = 0; id < users_.size(); ++id)
    {
        auto &user = users_[id];
        user.wake =
            config_.vus == 1 ? 0 : config_.ramp_up * static_cast<double>(id) / (config_.vus - 1);
        user.retire = config_.ramp_up + config_.hold +
                      config_.ramp_down * static_cast<double>(config_.vus - id) / config_.vus;
        schedule(id);
    }
}

void Load::prepare_sockets()
{
    const auto started = Clock::now();
    for (std::size_t id = 0; id < users_.size() && !interrupted; ++id)
    {
        auto socket = make_socket(id);
        if (socket.get() < 0)
        {
            throw std::runtime_error("prepare socket: " + std::string(std::strerror(errno)));
        }
        users_[id].prepared_socket = std::move(socket);
        ++metrics_.prepared_sockets;
    }
    metrics_.socket_preparation_seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
}

Metrics Load::run(bool progress)
{
    start_ = Clock::now();
    if (metrics_.diagnostics)
    {
        metrics_.diagnostics->workload_start_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(start_.time_since_epoch())
                .count());
    }
    double next_progress = 5;
    double previous_poll_return = 0;
    std::array<epoll_event, 512> events{};
    while (!timers_.empty() && !interrupted)
    {
        double now = elapsed();
        observe_connections(now);
        const auto timers_started = now;
        unsigned timers_serviced = 0;
        // A connect call can still stall, so check the time budget between timer services
        while (!timers_.empty() && timers_.due(timers_.first()) <= now &&
               timers_serviced < max_timer_batch && now - timers_started < timer_batch_seconds)
        {
            const auto id = timers_.first();
            if (metrics_.diagnostics)
            {
                metrics_.diagnostics->timer(now, timers_.due(id));
            }
            tick(id, now);
            if (!users_[id].stopped)
            {
                schedule(id);
            }
            now = elapsed();
            ++timers_serviced;
        }
        if (metrics_.diagnostics)
        {
            metrics_.diagnostics->timer_batch(timers_started, now, timers_serviced);
        }
        if (progress && now >= next_progress)
        {
            std::cout << "elapsed=" << std::fixed << std::setprecision(1) << now
                      << "s vus=" << active_ << " requests=" << metrics_.latencies.size()
                      << " failed=" << metrics_.failures << '\n'
                      << std::flush;
            next_progress = now + 5;
        }
        if (timers_.empty())
        {
            break;
        }
        const auto delay =
            std::max(0.0, std::min(timers_.due(timers_.first()), next_progress) - now);
        const int wait_ms = static_cast<int>(std::min(1000.0, std::ceil(delay * 1000)));
        if (metrics_.diagnostics)
        {
            // Exclude epoll's intentional wait, but include timer work and prior I/O dispatch
            metrics_.diagnostics->repoll(elapsed(), previous_poll_return);
        }
        const int count =
            ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), wait_ms);
        const double batch_ready = metrics_.diagnostics ? elapsed() : 0;
        previous_poll_return = batch_ready;
        if (count < 0 && errno != EINTR)
        {
            throw std::runtime_error("epoll_wait: " + std::string(std::strerror(errno)));
        }
        for (int index = 0; index < count; ++index)
        {
            const auto &event = events[static_cast<std::size_t>(index)];
            const auto id = static_cast<std::uint32_t>(event.data.u64);
            const auto generation = static_cast<std::uint32_t>(event.data.u64 >> 32);
            auto &user = users_[id];
            const double event_time = elapsed();
            if (metrics_.diagnostics)
            {
                metrics_.diagnostics->epoll(event_time, batch_ready);
            }
            if (!user.stopped && timers_.due(id) <= event_time)
            {
                if (metrics_.diagnostics)
                {
                    metrics_.diagnostics->timer(event_time, timers_.due(id));
                }
                tick(id, event_time);
                if (!user.stopped)
                {
                    schedule(id);
                }
            }
            if (!user.stopped && user.socket.get() >= 0 && user.generation == generation)
            {
                io(id, event.events, elapsed());
                if (!user.stopped)
                {
                    schedule(id);
                }
            }
        }
    }
    if (interrupted)
    {
        for (std::size_t id = 0; id < users_.size(); ++id)
        {
            if (!users_[id].stopped)
            {
                stop(id, users_[id].started);
            }
        }
    }
    metrics_.elapsed = elapsed();
    observe_connections(metrics_.elapsed);
    return std::move(metrics_);
}

double Load::elapsed() const
{
    return std::chrono::duration<double>(Clock::now() - start_).count();
}

double Load::between(double low, double high)
{
    return std::uniform_real_distribution<double>(low, high)(random_) * config_.time_scale;
}

bool Load::chance(double probability)
{
    return std::generate_canonical<double, 53>(random_) < probability;
}

void Load::observe_connections(double now)
{
    const double hold_end = config_.ramp_up + config_.hold;
    if ((now >= config_.ramp_up && now < hold_end) ||
        (last_connection_observation_ < hold_end && now > config_.ramp_up))
    {
        metrics_.hold_peak_live_connections =
            std::max(metrics_.hold_peak_live_connections, metrics_.live_connections);
    }
    last_connection_observation_ = now;
}

void Load::connection_established(std::size_t id, double now)
{
    auto &user = users_[id];
    observe_connections(now);
    user.socket_established = true;
    user.request->connecting = false;
    user.request->connected = now;
    ++metrics_.established_connections;
    ++metrics_.live_connections;
    metrics_.peak_live_connections =
        std::max(metrics_.peak_live_connections, metrics_.live_connections);
    observe_connections(now);
}

void Load::schedule(std::size_t id)
{
    auto &user = users_[id];
    auto due = user.request ? user.request->deadline : user.wake;
    if (user.request && user.request->connecting)
    {
        due = std::min(due, user.request->queued + 30);
    }
    due = std::min(due, user.retire + (user.retiring ? config_.grace : 0));
    timers_.update(id, due);
}

void Load::close(std::size_t id)
{
    auto &user = users_[id];
    if (user.socket.get() >= 0)
    {
        observe_connections(elapsed());
        if (user.socket_established)
        {
            user.socket_established = false;
            --metrics_.live_connections;
            ++metrics_.closed_connections;
        }
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, user.socket.get(), nullptr);
        user.socket.reset();
    }
}

void Load::interest(std::size_t id, bool write_ready, bool add)
{
    auto &user = users_[id];
    if (!add && user.write_interest == write_ready)
    {
        return;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | (write_ready ? static_cast<std::uint32_t>(EPOLLOUT) : 0U);
    event.data.u64 = (static_cast<std::uint64_t>(user.generation) << 32) | id;
    if (::epoll_ctl(epoll_.get(), add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, user.socket.get(), &event) <
        0)
    {
        throw std::runtime_error("epoll_ctl: " + std::string(std::strerror(errno)));
    }
    user.write_interest = write_ready;
}

void Load::stop(std::size_t id, bool iteration_interrupted)
{
    auto &user = users_[id];
    if (iteration_interrupted)
    {
        ++metrics_.interrupted_iterations;
    }
    if (user.request)
    {
        close(id);
    }
    else if (user.socket.get() >= 0)
    {
        // k6 retains idle per-VU transports after retirement until the run ends
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, user.socket.get(), nullptr);
    }
    user.request.reset();
    user.stopped = true;
    if (user.started)
    {
        --active_;
    }
    timers_.remove(id);
}

void Load::finish_iteration(std::size_t id, double now)
{
    auto &user = users_[id];
    ++metrics_.iterations;
    ++user.iteration;
    if (user.retiring || now >= user.retire)
    {
        stop(id, false);
    }
    else
    {
        user.step = Step::feed;
        user.wake = now;
    }
}

void Load::idle(User &user, double now)
{
    user.step = Step::finish;
    user.wake = now + between(5, 15);
}

void Load::create_or_idle(std::size_t id, double now)
{
    if (chance(0.02))
    {
        send_request(id, Step::create, now);
    }
    else
    {
        idle(users_[id], now);
    }
}

void Load::tick(std::size_t id, double now)
{
    auto &user = users_[id];
    if (now >= user.retire)
    {
        user.retiring = true;
        if (now >= user.retire + config_.grace)
        {
            stop(id, user.started);
            return;
        }
    }
    if (user.request)
    {
        if (now >= user.request->deadline)
        {
            complete(id, false, now, "request timeout");
        }
        else if (user.request->connecting && now >= user.request->queued + 30)
        {
            complete(id, false, now, "connect timeout");
        }
        return;
    }
    if (now < user.wake)
    {
        return;
    }
    if (!user.started)
    {
        user.started = true;
        ++active_;
        if (active_ == config_.vus && !metrics_.target_activation_seconds)
        {
            metrics_.target_activation_seconds = now;
        }
        user.wake = now + between(0, 5);
        return;
    }
    switch (user.step)
    {
    case Step::feed:
    case Step::post:
        send_request(id, user.step, now);
        break;
    case Step::actions:
        if (chance(0.15))
        {
            send_request(id, Step::like, now);
        }
        else
        {
            create_or_idle(id, now);
        }
        break;
    case Step::finish:
        finish_iteration(id, now);
        break;
    case Step::like:
    case Step::create:
        throw std::logic_error("unexpected sleeping request step");
    }
}

void Load::send_request(std::size_t id, Step step, double now)
{
    auto &user = users_[id];
    auto request = std::make_unique<Request>(step);
    request->queued = now;
    request->connected = now;
    request->deadline = now + config_.timeout;
    if (step == Step::feed)
    {
        request->wire = feed_request_;
    }
    else
    {
        const bool post = step == Step::like || step == Step::create;
        const auto &token = tokens_[id % tokens_.size()];
        const auto body =
            step == Step::create ? create_body(token, id + 1, user.iteration, timestamp()) : "";
        auto &outgoing = request->outgoing;
        outgoing.reserve(write_post_prefix_.size() + request_headers_.size() +
                         (post ? token.authorization.size() + body.size() + 96 : 24));
        outgoing += post ? write_post_prefix_ : get_post_prefix_;
        if (step != Step::create)
        {
            if (post)
            {
                outgoing += '/';
            }
            append_integer(outgoing, user.post_id);
            if (step == Step::like)
            {
                outgoing += "/like";
            }
        }
        outgoing += request_headers_;
        if (post)
        {
            outgoing += token.authorization;
            outgoing += "Content-Type: application/json\r\nContent-Length: ";
            append_integer(outgoing, body.size());
            outgoing += "\r\n\r\n";
            outgoing += body;
        }
        else
        {
            outgoing += "\r\n";
        }
        request->wire = outgoing;
    }
    user.request = std::move(request);
    if (user.socket.get() >= 0)
    {
        char byte = 0;
        const auto peek = ::recv(user.socket.get(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peek >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        {
            close(id);
        }
    }
    if (user.socket.get() < 0)
    {
        connect(id, now);
    }
    else
    {
        // Match the transport clock: request formatting and idle-socket probing are blocked time
        user.request->connected = elapsed();
        write(id, user.request->connected);
    }
}

Descriptor Load::make_socket(std::size_t id)
{
    Descriptor socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (socket.get() < 0)
    {
        return socket;
    }
    const int enabled = 1;
    if (::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
    {
        throw std::runtime_error("TCP_NODELAY: " + std::string(std::strerror(errno)));
    }
    if (!local_ips_.empty())
    {
        if (::setsockopt(socket.get(), IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &enabled,
                         sizeof(enabled)) != 0)
        {
            throw std::runtime_error("IP_BIND_ADDRESS_NO_PORT: " +
                                     std::string(std::strerror(errno)));
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr = local_ips_[id % local_ips_.size()];
        if (::bind(socket.get(), reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
        {
            throw std::runtime_error("bind local IPv4 address: " +
                                     std::string(std::strerror(errno)));
        }
    }

    return socket;
}

void Load::connect(std::size_t id, double now)
{
    auto &user = users_[id];
    user.request->connecting = true;
    user.socket = std::move(user.prepared_socket);
    if (user.socket.get() < 0)
    {
        user.socket = make_socket(id);
    }
    if (user.socket.get() < 0)
    {
        complete(id, false, now, std::strerror(errno));
        return;
    }
    ++user.generation;
    const int result =
        ::connect(user.socket.get(), reinterpret_cast<const sockaddr *>(&target_.address),
                  sizeof(target_.address));
    if (result != 0 && errno != EINPROGRESS)
    {
        complete(id, false, now, std::strerror(errno));
        return;
    }
    ++metrics_.connections;
    interest(id, true, true);
    if (result == 0)
    {
        connection_established(id, elapsed());
        write(id, elapsed());
    }
}

void Load::write(std::size_t id, double now)
{
    auto &user = users_[id];
    auto &request = *user.request;
    while (request.sent < request.wire.size())
    {
        const auto written = ::send(user.socket.get(), request.wire.data() + request.sent,
                                    request.wire.size() - request.sent, MSG_NOSIGNAL);
        if (written > 0)
        {
            request.sent += static_cast<std::size_t>(written);
        }
        else if (written < 0 && errno == EINTR)
        {
            continue;
        }
        else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            interest(id, true);
            return;
        }
        else
        {
            complete(id, false, now, std::strerror(errno));
            return;
        }
    }
    // Request strings and response bodies exist only during active I/O
    request.wire = {};
    std::string{}.swap(request.outgoing);
    interest(id, false);
}

void Load::io(std::size_t id, std::uint32_t events, double now)
{
    auto &user = users_[id];
    if (!user.request)
    {
        close(id);
        return;
    }
    const auto *original = user.request.get();
    if (user.request->connecting)
    {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(user.socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0)
        {
            error = errno;
        }
        if (error != 0)
        {
            complete(id, false, now, std::strerror(error));
            return;
        }
        connection_established(id, now);
        write(id, now);
        if (user.request.get() != original)
        {
            return;
        }
    }
    else if ((events & EPOLLOUT) != 0 && !user.request->wire.empty())
    {
        write(id, now);
        if (user.request.get() != original)
        {
            return;
        }
    }
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) == 0)
    {
        return;
    }
    std::array<char, 16384> buffer;
    std::size_t remaining = 256 * 1024;
    while (user.request && remaining > 0)
    {
        const double receive_time = elapsed();
        if (receive_time >= user.request->deadline || receive_time >= user.retire + config_.grace)
        {
            complete(id, false, receive_time, "request timeout");
            return;
        }
        const auto received =
            ::recv(user.socket.get(), buffer.data(), std::min(buffer.size(), remaining), 0);
        if (received > 0)
        {
            remaining -= static_cast<std::size_t>(received);
            metrics_.bytes_received += static_cast<std::uint64_t>(received);
            if (!user.request->receive({buffer.data(), static_cast<std::size_t>(received)}))
            {
                complete(id, false, elapsed(),
                         user.request->body_too_large ? "feed response exceeds 16 MiB"
                                                      : "invalid HTTP response framing");
                return;
            }
            if (user.request->complete)
            {
                complete(id, true, elapsed());
                return;
            }
        }
        else if (received == 0)
        {
            const bool valid = user.request->eof();
            close(id);
            complete(id, valid, elapsed(), valid ? "" : "EOF before complete HTTP response");
            return;
        }
        else if (errno == EINTR)
        {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return;
        }
        else
        {
            complete(id, false, elapsed(), std::strerror(errno));
            return;
        }
    }
}

void Load::complete(std::size_t id, bool transport_ok, double now, std::string_view error)
{
    auto &user = users_[id];
    if (transport_ok && user.request->complete && !user.received_http_response)
    {
        user.received_http_response = true;
        ++metrics_.vus_with_http_response;
    }
    if (now >= user.retire + config_.grace)
    {
        stop(id, user.started);
        return;
    }
    if (transport_ok && now >= user.request->deadline)
    {
        transport_ok = false;
        error = "request timeout";
    }
    auto request = std::move(user.request);
    const auto status = transport_ok ? request->parser.status_code : 0;
    const bool http_ok = status >= 200 && status <= 399;
    const double sample = request->connecting ? 0 : std::max(0.0, now - request->connected) * 1000;
    const auto check_index = request->step == Step::feed   ? 0U
                             : request->step == Step::post ? 1U
                             : request->step == Step::like ? 2U
                                                           : 3U;
    metrics_.latencies.record(sample, static_cast<EndpointKind>(check_index),
                              completion_phase(config_, now));
    if (metrics_.diagnostics)
    {
        metrics_.diagnostics->request(now, sample);
    }
    metrics_.blocked_sum += (request->connecting ? now : request->connected) - request->queued;
    if (!http_ok)
    {
        ++metrics_.failures;
        if (metrics_.failures <= 5)
        {
            std::cerr << "request failed: VU " << id + 1 << " status=" << status;
            if (!error.empty())
            {
                std::cerr << " " << error;
            }
            std::cerr << '\n';
        }
    }
    if (!transport_ok || !request->keep_alive)
    {
        close(id);
    }
    const bool check_ok = request->step == Step::create ? status == 201
                          : request->step == Step::like ? status == 200 || status == 201
                                                        : status == 200;
    ++metrics_.endpoint_requests[check_index];
    ++(check_ok ? metrics_.checks_passed : metrics_.checks_failed)[check_index];
    switch (request->step)
    {
    case Step::feed:
        if (!check_ok)
        {
            user.step = Step::finish;
            const auto think = between(3, 7);
            user.wake = elapsed() + think;
        }
        else if (const auto post = pick_post(request->body, random_))
        {
            user.post_id = *post;
            user.step = Step::post;
            // Parsing the actual feed happens before the browsing think time starts
            const auto think = between(3, 7);
            user.wake = elapsed() + think;
        }
        else
        {
            ++metrics_.workflow_errors;
            if (metrics_.workflow_errors <= 5)
            {
                std::cerr << "feed workflow error: expected nonempty posts array with unsigned "
                             "integer id\n";
            }
            finish_iteration(id, now);
        }
        break;
    case Step::post: {
        user.step = Step::actions;
        const auto think = between(3, 8);
        user.wake = elapsed() + think;
        break;
    }
    case Step::like:
        create_or_idle(id, elapsed());
        break;
    case Step::create:
        idle(user, elapsed());
        break;
    default:
        throw std::logic_error("completed non-request step");
    }
}

} // namespace loadgen
