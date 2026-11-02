#include <llhttp.h>
#include <yyjson.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::string_view version = "0.1.0";
constexpr std::size_t max_response_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_samples = 20'000'000;
volatile std::sig_atomic_t interrupted = 0;

void stop_signal(int)
{
    interrupted = 1;
}

class Descriptor
{
  public:
    explicit Descriptor(int value = -1) noexcept : value_(value)
    {
    }

    ~Descriptor()
    {
        reset();
    }

    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;

    Descriptor(Descriptor &&other) noexcept : value_(std::exchange(other.value_, -1))
    {
    }

    Descriptor &operator=(Descriptor &&other) noexcept
    {
        if (this != &other)
        {
            reset(std::exchange(other.value_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return value_;
    }

    void reset(int value = -1) noexcept
    {
        if (value_ >= 0)
        {
            ::close(value_);
        }
        value_ = value;
    }

  private:
    int value_;
};

using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

Document json(std::string_view text)
{
    return {yyjson_read(text.data(), text.size(), 0), &yyjson_doc_free};
}

std::string json_string(std::string_view value)
{
    std::string output = "\"";
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            output += '\\';
            output += static_cast<char>(c);
        }
        else if (c < 32)
        {
            output += "\\u00";
            output += "0123456789abcdef"[c >> 4];
            output += "0123456789abcdef"[c & 15];
        }
        else
        {
            output += static_cast<char>(c);
        }
    }
    return output + '"';
}

std::string environment(const char *name, std::string fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : std::move(fallback);
}

unsigned integer(std::string_view text, unsigned maximum, std::string_view name)
{
    unsigned result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result == 0 || result > maximum)
    {
        throw std::runtime_error(std::string(name) + " must be between 1 and " +
                                 std::to_string(maximum));
    }
    return result;
}

double duration(std::string_view text)
{
    if (text.empty())
    {
        throw std::runtime_error("duration cannot be empty");
    }
    double total = 0;
    while (!text.empty())
    {
        double number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || end == text.data() || !std::isfinite(number) || number < 0)
        {
            throw std::runtime_error("invalid duration, use units such as 500ms, 30s or 5m");
        }
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        double scale = 1;
        if (text.starts_with("ms"))
        {
            scale = 0.001;
            text.remove_prefix(2);
        }
        else if (text.starts_with('s'))
        {
            text.remove_prefix(1);
        }
        else if (text.starts_with('m'))
        {
            scale = 60;
            text.remove_prefix(1);
        }
        else if (text.starts_with('h'))
        {
            scale = 3600;
            text.remove_prefix(1);
        }
        else
        {
            throw std::runtime_error("duration requires ms, s, m or h units");
        }
        total += number * scale;
    }
    if (!std::isfinite(total) || total > 7 * 24 * 3600)
    {
        throw std::runtime_error("duration exceeds seven days");
    }
    return total;
}

struct Config
{
    std::string base_url = environment("BASE_URL", "http://127.0.0.1:3000");
    std::string tokens = environment("TOKENS", "seed/tokens.json");
    std::string summary;
    std::string ready_file;
    std::string start_gate;
    std::string local_ips;
    unsigned vus = integer(environment("VUS", "100"), 1'000'000, "VUS");
    double hold = duration(environment("DURATION", "5m"));
    double ramp_up = duration(environment("RAMP_UP", "60s"));
    double ramp_down = 30;
    double grace = 10;
    double timeout = 60;
    double time_scale = 1;
    std::uint64_t seed = std::random_device{}();
};

struct Endpoint
{
    sockaddr_in address{};
    std::string authority;
    std::string prefix;
};

Endpoint endpoint(std::string_view url)
{
    if (!url.starts_with("http://"))
    {
        throw std::runtime_error("BASE_URL supports plain http:// with IPv4 only");
    }
    url.remove_prefix(7);
    const auto slash = url.find('/');
    const auto authority = url.substr(0, slash);
    if (authority.empty() || authority.find_first_of("@[]?#\r\n ") != std::string_view::npos)
    {
        throw std::runtime_error("BASE_URL requires an IPv4 host without credentials or query");
    }
    const auto colon = authority.find(':');
    const std::string host(authority.substr(0, colon));
    const std::string port =
        colon == std::string_view::npos ? "80" : std::string(authority.substr(colon + 1));
    static_cast<void>(integer(port, 65535, "port"));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *raw = nullptr;
    const int result = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &raw);
    if (result != 0)
    {
        throw std::runtime_error("IPv4 lookup failed: " + std::string(gai_strerror(result)));
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, &freeaddrinfo);
    Endpoint output;
    std::memcpy(&output.address, raw->ai_addr, sizeof(output.address));
    output.authority = authority;
    if (slash != std::string_view::npos)
    {
        output.prefix = url.substr(slash);
        if (output.prefix.find_first_of("?#\r\n ") != std::string::npos)
        {
            throw std::runtime_error("BASE_URL path cannot contain query, fragment or whitespace");
        }
        while (output.prefix.ends_with('/'))
        {
            output.prefix.pop_back();
        }
    }
    return output;
}

std::vector<in_addr> local_addresses(std::string_view input)
{
    std::vector<in_addr> result;
    while (!input.empty())
    {
        const auto comma = input.find(',');
        const auto item = input.substr(0, comma);
        const auto dash = item.find('-');
        in_addr first{};
        in_addr last{};
        if (::inet_pton(AF_INET, std::string(item.substr(0, dash)).c_str(), &first) != 1 ||
            (dash != std::string_view::npos &&
             ::inet_pton(AF_INET, std::string(item.substr(dash + 1)).c_str(), &last) != 1))
        {
            throw std::runtime_error("--local-ips requires IPv4 addresses or IPv4-IPv4 ranges");
        }
        const auto begin = ntohl(first.s_addr);
        const auto end = dash == std::string_view::npos ? begin : ntohl(last.s_addr);
        if (end < begin || static_cast<std::uint64_t>(end) - begin + result.size() >= 65536)
        {
            throw std::runtime_error("--local-ips range is reversed or exceeds 65536 addresses");
        }
        for (std::uint64_t address = begin; address <= end; ++address)
        {
            result.push_back(in_addr{htonl(static_cast<std::uint32_t>(address))});
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        input.remove_prefix(comma + 1);
        if (input.empty())
        {
            throw std::runtime_error("--local-ips has an empty address");
        }
    }
    return result;
}

struct Token
{
    std::string username;
    std::string authorization;
};

std::vector<Token> read_tokens(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open token file: " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const auto document = json(text);
    auto *root = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_arr(root) || yyjson_arr_size(root) == 0)
    {
        throw std::runtime_error("token file must contain a nonempty JSON array");
    }
    std::vector<Token> tokens;
    tokens.reserve(yyjson_arr_size(root));
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val *entry = nullptr;
    yyjson_arr_foreach(root, index, count, entry)
    {
        auto *username = yyjson_obj_get(entry, "username");
        auto *token = yyjson_obj_get(entry, "token");
        if (!yyjson_is_str(username) || !yyjson_is_str(token) || yyjson_get_len(token) == 0)
        {
            throw std::runtime_error("each token needs string username and nonempty token");
        }
        const std::string_view value(yyjson_get_str(token), yyjson_get_len(token));
        if (value.find_first_of("\r\n") != std::string_view::npos)
        {
            throw std::runtime_error("token cannot contain HTTP header line breaks");
        }
        tokens.push_back({std::string(yyjson_get_str(username), yyjson_get_len(username)),
                          "Authorization: Bearer " + std::string(value) + "\r\n"});
    }
    return tokens;
}

enum class Step : std::uint8_t
{
    feed,
    post,
    actions,
    like,
    create,
    finish
};

struct Request
{
    llhttp_t parser{};
    std::string outgoing;
    std::string body;
    std::size_t sent = 0;
    std::size_t received = 0;
    double queued = 0;
    double connected = 0;
    double deadline = 0;
    Step step;
    bool connecting = false;
    bool complete = false;
    bool keep_alive = false;
    bool body_too_large = false;

    explicit Request(Step value) : step(value)
    {
        llhttp_init(&parser, HTTP_RESPONSE, &settings());
        parser.data = this;
    }

    static const llhttp_settings_t &settings()
    {
        static const llhttp_settings_t value = [] {
            llhttp_settings_t setup{};
            llhttp_settings_init(&setup);
            setup.on_body = [](llhttp_t *parser, const char *data, std::size_t size) {
                auto &request = *static_cast<Request *>(parser->data);
                if (request.step == Step::feed)
                {
                    if (size > max_response_bytes - request.body.size())
                    {
                        request.body_too_large = true;
                        return -1;
                    }
                    request.body.append(data, size);
                }
                return 0;
            };
            setup.on_message_complete = [](llhttp_t *parser) {
                auto &request = *static_cast<Request *>(parser->data);
                if (parser->status_code < 200 && parser->status_code != 101)
                {
                    return 0;
                }
                request.complete = true;
                request.keep_alive = llhttp_should_keep_alive(parser) != 0;
                return static_cast<int>(HPE_PAUSED);
            };
            return setup;
        }();
        return value;
    }

    [[nodiscard]] bool receive(std::string_view bytes)
    {
        received += bytes.size();
        const auto result = llhttp_execute(&parser, bytes.data(), bytes.size());
        if (result == HPE_PAUSED && complete)
        {
            // A sequential client never accepts bytes belonging to another final response
            return llhttp_get_error_pos(&parser) == bytes.data() + bytes.size();
        }
        return result == HPE_OK;
    }

    [[nodiscard]] bool eof()
    {
        const auto result = llhttp_finish(&parser);
        return complete && (result == HPE_OK || result == HPE_PAUSED);
    }
};

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

struct Metrics
{
    std::vector<double> durations;
    std::uint64_t failures = 0;
    std::array<std::uint64_t, 4> checks_passed{};
    std::array<std::uint64_t, 4> checks_failed{};
    std::array<std::uint64_t, 4> endpoint_requests{};
    std::uint64_t iterations = 0;
    std::uint64_t interrupted_iterations = 0;
    std::uint64_t workflow_errors = 0;
    std::uint64_t connections = 0;
    std::uint64_t bytes_received = 0;
    double blocked_sum = 0;
    double elapsed = 0;
};

struct User
{
    Descriptor socket;
    std::unique_ptr<Request> request;
    std::uint64_t iteration = 0;
    std::uint64_t post_id = 0;
    double wake = 0;
    double retire = 0;
    double timer = 0;
    std::size_t heap_position = 0;
    std::uint32_t generation = 0;
    Step step = Step::feed;
    bool started = false;
    bool retiring = false;
    bool stopped = false;
};

// Exactly one timer entry per VU, including while an HTTP request is active
class Timers
{
  public:
    explicit Timers(std::vector<User> &users) : users_(users)
    {
        heap_.reserve(users.size());
        for (std::size_t id = 0; id < users.size(); ++id)
        {
            users[id].heap_position = heap_.size();
            heap_.push_back(id);
            up(heap_.size() - 1);
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return heap_.empty();
    }

    [[nodiscard]] std::size_t first() const
    {
        return heap_.front();
    }

    void update(std::size_t id)
    {
        const auto position = users_[id].heap_position;
        up(position);
        down(users_[id].heap_position);
    }

    void remove(std::size_t id)
    {
        const auto position = users_[id].heap_position;
        exchange(position, heap_.size() - 1);
        heap_.pop_back();
        if (position < heap_.size())
        {
            const auto moved = heap_[position];
            up(position);
            down(users_[moved].heap_position);
        }
    }

  private:
    bool earlier(std::size_t left, std::size_t right) const
    {
        const auto a = heap_[left];
        const auto b = heap_[right];
        return users_[a].timer < users_[b].timer || (users_[a].timer == users_[b].timer && a < b);
    }

    void exchange(std::size_t left, std::size_t right)
    {
        std::swap(heap_[left], heap_[right]);
        users_[heap_[left]].heap_position = left;
        users_[heap_[right]].heap_position = right;
    }

    void up(std::size_t position)
    {
        while (position > 0 && earlier(position, (position - 1) / 2))
        {
            exchange(position, (position - 1) / 2);
            position = (position - 1) / 2;
        }
    }

    void down(std::size_t position)
    {
        while (position * 2 + 1 < heap_.size())
        {
            auto child = position * 2 + 1;
            if (child + 1 < heap_.size() && earlier(child + 1, child))
            {
                ++child;
            }
            if (!earlier(child, position))
            {
                break;
            }
            exchange(position, child);
            position = child;
        }
    }

    std::vector<User> &users_;
    std::vector<std::size_t> heap_;
};

class Load
{
  public:
    Load(Config config, Endpoint target, std::vector<Token> tokens)
        : config_(std::move(config)), target_(std::move(target)), tokens_(std::move(tokens)),
          local_ips_(local_addresses(config_.local_ips)), random_(config_.seed),
          epoll_(::epoll_create1(EPOLL_CLOEXEC)), users_(config_.vus), timers_(users_)
    {
        if (epoll_.get() < 0)
        {
            throw std::runtime_error("epoll_create1: " + std::string(std::strerror(errno)));
        }
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || config_.vus + 16 > limit.rlim_cur)
        {
            throw std::runtime_error(
                "file descriptor limit is too low; set ulimit -n above VUS + 16");
        }
        for (std::size_t id = 0; id < users_.size(); ++id)
        {
            auto &user = users_[id];
            user.wake = config_.vus == 1
                            ? 0
                            : config_.ramp_up * static_cast<double>(id) / (config_.vus - 1);
            user.retire = config_.ramp_up + config_.hold +
                          config_.ramp_down * static_cast<double>(config_.vus - id) / config_.vus;
            schedule(id);
        }
    }

    Metrics run(bool progress = true)
    {
        start_ = Clock::now();
        double next_progress = 5;
        std::array<epoll_event, 512> events{};
        while (!timers_.empty() && !interrupted)
        {
            double now = elapsed();
            while (!timers_.empty() && users_[timers_.first()].timer <= now)
            {
                const auto id = timers_.first();
                tick(id, now);
                if (!users_[id].stopped)
                {
                    schedule(id);
                }
                now = elapsed();
            }
            if (progress && now >= next_progress)
            {
                std::cout << "elapsed=" << std::fixed << std::setprecision(1) << now
                          << "s vus=" << active_ << " requests=" << metrics_.durations.size()
                          << " failed=" << metrics_.failures << '\n'
                          << std::flush;
                next_progress = now + 5;
            }
            if (timers_.empty())
            {
                break;
            }
            const auto delay =
                std::max(0.0, std::min(users_[timers_.first()].timer, next_progress) - now);
            const int wait_ms = static_cast<int>(std::min(1000.0, std::ceil(delay * 1000)));
            const int count =
                ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), wait_ms);
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
                if (!user.stopped && user.timer <= event_time)
                {
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
        return std::move(metrics_);
    }

  private:
    [[nodiscard]] double elapsed() const
    {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

    double between(double low, double high)
    {
        return std::uniform_real_distribution<double>(low, high)(random_) * config_.time_scale;
    }

    bool chance(double probability)
    {
        return std::generate_canonical<double, 53>(random_) < probability;
    }

    void schedule(std::size_t id)
    {
        auto &user = users_[id];
        user.timer = user.request ? user.request->deadline : user.wake;
        if (user.request && user.request->connecting)
        {
            user.timer = std::min(user.timer, user.request->queued + 30);
        }
        user.timer = std::min(user.timer, user.retire + (user.retiring ? config_.grace : 0));
        timers_.update(id);
    }

    void close(std::size_t id)
    {
        auto &user = users_[id];
        if (user.socket.get() >= 0)
        {
            ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, user.socket.get(), nullptr);
            user.socket.reset();
        }
    }

    void interest(std::size_t id, bool write_ready, bool add = false)
    {
        auto &user = users_[id];
        epoll_event event{};
        event.events =
            EPOLLIN | EPOLLRDHUP | (write_ready ? static_cast<std::uint32_t>(EPOLLOUT) : 0U);
        event.data.u64 = (static_cast<std::uint64_t>(user.generation) << 32) | id;
        if (::epoll_ctl(epoll_.get(), add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, user.socket.get(),
                        &event) < 0)
        {
            throw std::runtime_error("epoll_ctl: " + std::string(std::strerror(errno)));
        }
    }

    void stop(std::size_t id, bool iteration_interrupted)
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

    void finish_iteration(std::size_t id, double now)
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

    void idle(User &user, double now)
    {
        user.step = Step::finish;
        user.wake = now + between(5, 15);
    }

    void create_or_idle(std::size_t id, double now)
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

    void tick(std::size_t id, double now)
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

    void send_request(std::size_t id, Step step, double now)
    {
        auto &user = users_[id];
        auto request = std::make_unique<Request>(step);
        request->queued = now;
        request->connected = now;
        request->deadline = now + config_.timeout;
        const bool post = step == Step::like || step == Step::create;
        std::string path = target_.prefix;
        if (step == Step::feed)
        {
            path += "/feed";
        }
        else if (step == Step::create)
        {
            path += "/posts";
        }
        else
        {
            path += "/posts/" + std::to_string(user.post_id);
            if (step == Step::like)
            {
                path += "/like";
            }
        }
        request->outgoing =
            std::string(post ? "POST " : "GET ") + path +
            " HTTP/1.1\r\nHost: " + target_.authority +
            "\r\nUser-Agent: native-load/0.1.0\r\nAccept: */*\r\nConnection: keep-alive\r\n";
        if (post)
        {
            const auto &token = tokens_[id % tokens_.size()];
            const std::string body =
                step == Step::create ? create_body(token, id + 1, user.iteration, timestamp()) : "";
            request->outgoing +=
                token.authorization +
                "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\n\r\n" + body;
        }
        else
        {
            request->outgoing += "\r\n";
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
            write(id, now);
        }
    }

    void connect(std::size_t id, double now)
    {
        auto &user = users_[id];
        user.request->connecting = true;
        user.socket.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (user.socket.get() < 0)
        {
            complete(id, false, now, std::strerror(errno));
            return;
        }
        ++user.generation;
        const int enabled = 1;
        if (::setsockopt(user.socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) !=
            0)
        {
            throw std::runtime_error("TCP_NODELAY: " + std::string(std::strerror(errno)));
        }
        if (!local_ips_.empty())
        {
            if (::setsockopt(user.socket.get(), IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &enabled,
                             sizeof(enabled)) != 0)
            {
                throw std::runtime_error("IP_BIND_ADDRESS_NO_PORT: " +
                                         std::string(std::strerror(errno)));
            }
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr = local_ips_[id % local_ips_.size()];
            if (::bind(user.socket.get(), reinterpret_cast<const sockaddr *>(&local),
                       sizeof(local)) != 0)
            {
                throw std::runtime_error("bind local IPv4 address: " +
                                         std::string(std::strerror(errno)));
            }
        }
        user.request->connecting = true;
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
            user.request->connecting = false;
            user.request->connected = elapsed();
            write(id, elapsed());
        }
    }

    void write(std::size_t id, double now)
    {
        auto &user = users_[id];
        auto &request = *user.request;
        while (request.sent < request.outgoing.size())
        {
            const auto written = ::send(user.socket.get(), request.outgoing.data() + request.sent,
                                        request.outgoing.size() - request.sent, MSG_NOSIGNAL);
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
        std::string{}.swap(request.outgoing);
        interest(id, false);
    }

    void io(std::size_t id, std::uint32_t events, double now)
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
            user.request->connecting = false;
            user.request->connected = now;
            write(id, now);
            if (user.request.get() != original)
            {
                return;
            }
        }
        else if ((events & EPOLLOUT) != 0 && !user.request->outgoing.empty())
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
        std::array<char, 16384> buffer{};
        std::size_t remaining = 256 * 1024;
        while (user.request && remaining > 0)
        {
            const double receive_time = elapsed();
            if (receive_time >= user.request->deadline ||
                receive_time >= user.retire + config_.grace)
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

    void complete(std::size_t id, bool transport_ok, double now, std::string_view error = {})
    {
        auto &user = users_[id];
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
        if (metrics_.durations.size() == max_samples)
        {
            throw std::runtime_error("exact duration sample limit reached (20 million requests, "
                                     "160 MiB); shorten the run");
        }
        if (metrics_.durations.size() == metrics_.durations.capacity())
        {
            metrics_.durations.reserve(std::min(
                max_samples, std::max(std::size_t{4096}, metrics_.durations.capacity() * 2)));
        }
        metrics_.durations.push_back(
            request->connecting ? 0 : std::max(0.0, now - request->connected) * 1000);
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
        const auto check_index = request->step == Step::feed   ? 0U
                                 : request->step == Step::post ? 1U
                                 : request->step == Step::like ? 2U
                                                               : 3U;
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
                user.wake = now + between(3, 7);
            }
            else if (const auto post = pick_post(request->body, random_))
            {
                user.post_id = *post;
                user.step = Step::post;
                user.wake = now + between(3, 7);
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
        case Step::post:
            user.step = Step::actions;
            user.wake = now + between(3, 8);
            break;
        case Step::like:
            create_or_idle(id, now);
            break;
        case Step::create:
            idle(user, now);
            break;
        default:
            throw std::logic_error("completed non-request step");
        }
    }

    Config config_;
    Endpoint target_;
    std::vector<Token> tokens_;
    std::vector<in_addr> local_ips_;
    std::mt19937_64 random_;
    Descriptor epoll_;
    std::vector<User> users_;
    Timers timers_;
    Metrics metrics_;
    Clock::time_point start_{};
    unsigned active_ = 0;
};

double percentile(const std::vector<double> &sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0;
    }
    const double rank = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(rank);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * (rank - static_cast<double>(lower));
}

struct Summary
{
    std::string json;
    bool passed;
};

Summary summarize(Metrics &metrics, const Config &config)
{
    std::sort(metrics.durations.begin(), metrics.durations.end());
    const auto count = metrics.durations.size();
    double sum = 0;
    for (const double sample : metrics.durations)
    {
        sum += sample;
    }
    std::uint64_t passes = 0;
    std::uint64_t fails = 0;
    for (std::size_t index = 0; index < metrics.checks_passed.size(); ++index)
    {
        passes += metrics.checks_passed[index];
        fails += metrics.checks_failed[index];
    }
    const double p95 = percentile(metrics.durations, 0.95);
    const double p99 = percentile(metrics.durations, 0.99);
    const double failure_rate =
        count == 0 ? 0 : static_cast<double>(metrics.failures) / static_cast<double>(count);
    const bool passed = p95 < 500 && p99 < 1000 && failure_rate < 0.01;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
    {
        throw std::runtime_error("getrusage: " + std::string(std::strerror(errno)));
    }
    std::ostringstream output;
    output << std::setprecision(12) << std::boolalpha;
    output << "{\n  \"metrics\": {\n    \"http_reqs\": {\"count\":" << count << ",\"rate\":"
           << (metrics.elapsed > 0 ? static_cast<double>(count) / metrics.elapsed : 0) << "},\n"
           << "    \"http_req_duration\": {\"avg\":"
           << (count ? sum / static_cast<double>(count) : 0)
           << ",\"med\":" << percentile(metrics.durations, 0.5) << ",\"p(95)\":" << p95
           << ",\"p(99)\":" << p99 << ",\"max\":" << (count ? metrics.durations.back() : 0)
           << ",\"thresholds\":{\"p(95)<500\":" << (p95 < 500) << ",\"p(99)<1000\":" << (p99 < 1000)
           << "}},\n"
           << "    \"http_req_failed\": {\"value\":" << failure_rate
           << ",\"passes\":" << metrics.failures << ",\"fails\":" << count - metrics.failures
           << ",\"thresholds\":{\"rate<0.01\":" << (failure_rate < 0.01) << "}},\n"
           << "    \"checks\": {\"passes\":" << passes << ",\"fails\":" << fails << ",\"value\":"
           << (passes + fails ? static_cast<double>(passes) / static_cast<double>(passes + fails)
                              : 0)
           << "},\n"
           << "    \"iterations\": {\"count\":" << metrics.iterations << ",\"rate\":"
           << (metrics.elapsed > 0 ? static_cast<double>(metrics.iterations) / metrics.elapsed : 0)
           << "},\n"
           << "    \"http_req_blocked\": {\"avg\":"
           << (count ? metrics.blocked_sum * 1000 / static_cast<double>(count) : 0) << "}\n  },\n"
           << "  \"generator\": {\"name\":\"td-load\",\"version\":" << json_string(version)
           << ",\"transport\":\"epoll/http1.1/IPv4\",\"source\":\"bench/load.js\",\"passed\":"
           << passed << ",\"checks_passed\":" << (fails == 0) << ",\"vus\":" << config.vus
           << ",\"ramp_up_seconds\":" << config.ramp_up << ",\"hold_seconds\":" << config.hold
           << ",\"ramp_down_seconds\":" << config.ramp_down << ",\"grace_seconds\":" << config.grace
           << ",\"request_timeout_seconds\":" << config.timeout
           << ",\"elapsed_seconds\":" << metrics.elapsed
           << ",\"connections\":" << metrics.connections
           << ",\"max_rss_bytes\":" << static_cast<std::uint64_t>(usage.ru_maxrss) * 1024
           << ",\"idle_vu_state_bytes\":" << sizeof(User) + sizeof(std::size_t)
           << ",\"interrupted_iterations\":" << metrics.interrupted_iterations
           << ",\"workflow_errors\":" << metrics.workflow_errors
           << ",\"bytes_received\":" << metrics.bytes_received << ",\"seed\":" << config.seed
           << ",\"quantiles\":\"exact linear interpolation, all completed requests, 8 "
              "bytes/sample, 20 million sample limit\""
           << ",\"duration_semantics\":\"send+wait+receive, excludes connect; interrupted requests "
              "omitted\""
           << ",\"endpoint_requests\":{\"feed\":" << metrics.endpoint_requests[0]
           << ",\"post\":" << metrics.endpoint_requests[1]
           << ",\"like\":" << metrics.endpoint_requests[2]
           << ",\"create\":" << metrics.endpoint_requests[3] << "}}\n}\n";
    return {output.str(), passed};
}

void write_file(const std::string &path, std::string_view value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write file: " + path);
    }
}

void self_test();

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        {
            self_test();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version")
        {
            std::cout << "td-load " << version << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout
                << "load_generator [--base-url http://HOST:PORT] [--vus N] [--duration 5m]\n"
                   "  [--ramp-up 60s] [--ramp-down 30s] [--graceful-ramp-down 10s]\n"
                   "  [--request-timeout 60s] [--tokens seed/tokens.json] [--summary-export PATH]\n"
                   "  [--local-ips IPv4,IPv4-IPv4] [--ready-file PATH] [--start-gate PATH]\n"
                   "Environment: BASE_URL, VUS, DURATION, RAMP_UP, TOKENS\n"
                   "Linux, plain HTTP/1.1, IPv4; one persistent socket per VU\n"
                   "Exact quantiles retain 8 bytes/request (20 million request limit)\n";
            return 0;
        }
        Config config;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            const auto equals = argument.find('=');
            const auto name = argument.substr(0, equals);
            std::string_view value;
            if (equals != std::string_view::npos)
            {
                value = argument.substr(equals + 1);
            }
            else
            {
                if (index + 1 == argc)
                {
                    throw std::runtime_error("missing value for " + std::string(name));
                }
                value = argv[++index];
            }
            if (name == "--base-url")
            {
                config.base_url = value;
            }
            else if (name == "--vus")
            {
                config.vus = integer(value, 1'000'000, "VUS");
            }
            else if (name == "--duration")
            {
                config.hold = duration(value);
            }
            else if (name == "--ramp-up")
            {
                config.ramp_up = duration(value);
            }
            else if (name == "--ramp-down")
            {
                config.ramp_down = duration(value);
            }
            else if (name == "--graceful-ramp-down")
            {
                config.grace = duration(value);
            }
            else if (name == "--request-timeout")
            {
                config.timeout = duration(value);
            }
            else if (name == "--tokens")
            {
                config.tokens = value;
            }
            else if (name == "--summary-export")
            {
                config.summary = value;
            }
            else if (name == "--local-ips")
            {
                config.local_ips = value;
            }
            else if (name == "--ready-file")
            {
                config.ready_file = value;
            }
            else if (name == "--start-gate")
            {
                config.start_gate = value;
            }
            else
            {
                throw std::runtime_error("unsupported option: " + std::string(name));
            }
        }
        if (config.hold <= 0 || config.timeout <= 0)
        {
            throw std::runtime_error("DURATION and request timeout must be greater than zero");
        }
        std::signal(SIGINT, &stop_signal);
        std::signal(SIGTERM, &stop_signal);
        Load load(config, endpoint(config.base_url), read_tokens(config.tokens));
        if (!config.ready_file.empty())
        {
            write_file(config.ready_file, "ready\n");
        }
        if (!config.start_gate.empty())
        {
            std::cout << "ready, waiting for start gate\n" << std::flush;
            while (!std::filesystem::exists(config.start_gate) && !interrupted)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        auto metrics = load.run();
        const auto summary = summarize(metrics, config);
        if (!config.summary.empty())
        {
            write_file(config.summary, summary.json);
        }
        std::cout << summary.json;
        return interrupted ? 130 : summary.passed ? 0 : 99;
    }
    catch (const std::exception &error)
    {
        std::cerr << "load_generator: " << error.what() << '\n';
        return 1;
    }
}

namespace
{
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
        if (mode_ == FixtureMode::stalled)
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

Metrics fixture_run(Fixture &fixture, Config config)
{
    std::vector<Token> tokens;
    for (unsigned index = 0; index < 3; ++index)
    {
        tokens.push_back({"user" + std::to_string(index),
                          "Authorization: Bearer token" + std::to_string(index) + "\r\n"});
    }
    Load load(config, fixture.target(), std::move(tokens));
    auto metrics = load.run(false);
    fixture.check();
    return metrics;
}

void self_test()
{
    require(duration("1m500ms") == 60.5, "compound duration");
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
    Config config;
    config.vus = 4;
    config.ramp_up = 0.005;
    config.hold = 0.5;
    config.ramp_down = 0.005;
    config.grace = 0.04;
    config.timeout = 0.1;
    config.time_scale = 0.0001;
    config.local_ips = "127.0.0.2-127.0.0.5";
    config.seed = 42;
    Fixture workflow(FixtureMode::workflow);
    const auto metrics = fixture_run(workflow, config);
    require(metrics.failures == 0 && metrics.workflow_errors == 0, "live workflow HTTP");
    require(metrics.endpoint_requests[0] > 0 && metrics.endpoint_requests[1] > 0 &&
                metrics.endpoint_requests[2] > 0 && metrics.endpoint_requests[3] > 0,
            "all workflow branches");
    require(workflow.connections() == config.vus && metrics.connections == config.vus,
            "independent persistent VU sockets");
    require(metrics.interrupted_iterations == 0 && metrics.iterations > 0,
            "retirement finishes iteration");
    Fixture failed_feed(FixtureMode::failed_feed);
    config.hold = 0.04;
    const auto failed = fixture_run(failed_feed, config);
    require(failed.failures > 0 && failed.endpoint_requests[1] == 0 &&
                failed.endpoint_requests[2] == 0 && failed.endpoint_requests[3] == 0,
            "failed feed ends iteration");
    Fixture closed(FixtureMode::closed);
    config.vus = 1;
    config.hold = 0.04;
    const auto reconnected = fixture_run(closed, config);
    require(reconnected.failures == 0 && reconnected.connections > 1 &&
                reconnected.endpoint_requests[1] > 0,
            "EOF response reconnects");
    Fixture stalled(FixtureMode::stalled);
    config.hold = 0.01;
    config.ramp_up = 0;
    config.ramp_down = 0;
    config.grace = 0.005;
    const auto canceled = fixture_run(stalled, config);
    require(canceled.interrupted_iterations == 1 && canceled.durations.empty() &&
                canceled.failures == 0 && canceled.iterations == 0,
            "grace cancellation omits in-flight request");
    Fixture timeout(FixtureMode::stalled);
    config.hold = 0.025;
    config.timeout = 0.005;
    const auto timed_out = fixture_run(timeout, config);
    require(timed_out.failures > 0 && timed_out.checks_failed[0] > 0 &&
                timed_out.durations.front() >= 5,
            "request timeout is a real failed sample");
    std::cout << "load parser, timer, payload, quantile, socket and workflow self-tests passed\n";
}
} // namespace
