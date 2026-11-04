#pragma once

#include "config.hpp"
#include "inputs.hpp"
#include "metrics.hpp"

#include <llhttp.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace loadgen
{
extern volatile std::sig_atomic_t interrupted;
void stop_signal(int);

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
    double queued = 0;
    double connected = 0;
    double deadline = 0;
    Step step;
    bool connecting = false;
    bool complete = false;
    bool keep_alive = false;
    bool body_too_large = false;

    // llhttp points back to this object, so each active request keeps a stable address
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

std::optional<std::uint64_t> pick_post(std::string_view body, std::mt19937_64 &random);
std::string timestamp();
std::string create_body(const Token &token, std::size_t vu, std::uint64_t iteration,
                        std::string_view when);
Phase completion_phase(const RuntimeConfig &config, double completed);

struct User
{
    Descriptor socket;
    std::unique_ptr<Request> request;
    std::uint64_t iteration = 0;
    std::uint64_t post_id = 0;
    double wake = 0;
    double retire = 0;
    std::uint32_t generation = 0;
    Step step = Step::feed;
    bool started = false;
    bool retiring = false;
    bool stopped = false;
    bool socket_established = false;
    bool received_http_response = false;
    bool write_interest = false;
};

// Exactly one timer entry per VU, including while an HTTP request is active
class Timers
{
    // Keep scheduler metadata together instead of walking the transport state
    struct Entry
    {
        double due = 0;
        std::size_t position = 0;
    };

  public:
    static constexpr std::size_t per_user_bytes = sizeof(Entry) + sizeof(std::size_t);

    explicit Timers(std::size_t count) : entries_(count)
    {
        heap_.reserve(count);
        for (std::size_t id = 0; id < count; ++id)
        {
            entries_[id].position = id;
            heap_.push_back(id);
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

    [[nodiscard]] double due(std::size_t id) const
    {
        return entries_[id].due;
    }

    void update(std::size_t id, double due)
    {
        const auto previous = entries_[id].due;
        entries_[id].due = due;
        const auto position = entries_[id].position;
        if (due < previous)
        {
            up(position);
        }
        else if (due > previous)
        {
            down(position);
        }
    }

    void remove(std::size_t id)
    {
        const auto position = entries_[id].position;
        const auto moved = heap_.back();
        heap_.pop_back();
        if (position < heap_.size())
        {
            place(position, moved);
            if (!up(position))
            {
                down(position);
            }
        }
    }

  private:
    bool earlier(std::size_t a, std::size_t b) const
    {
        return entries_[a].due < entries_[b].due || (entries_[a].due == entries_[b].due && a < b);
    }

    void place(std::size_t position, std::size_t id)
    {
        heap_[position] = id;
        entries_[id].position = position;
    }

    bool up(std::size_t position)
    {
        const auto original = position;
        const auto id = heap_[position];
        while (position > 0)
        {
            const auto parent = (position - 1) / 2;
            if (!earlier(id, heap_[parent]))
            {
                break;
            }
            place(position, heap_[parent]);
            position = parent;
        }
        place(position, id);
        return position != original;
    }

    void down(std::size_t position)
    {
        const auto id = heap_[position];
        while (position * 2 + 1 < heap_.size())
        {
            auto child = position * 2 + 1;
            if (child + 1 < heap_.size() && earlier(heap_[child + 1], heap_[child]))
            {
                ++child;
            }
            if (!earlier(heap_[child], id))
            {
                break;
            }
            place(position, heap_[child]);
            position = child;
        }
        place(position, id);
    }

    std::vector<Entry> entries_;
    std::vector<std::size_t> heap_;
};

class Load
{
  public:
    Load(RuntimeConfig config, Endpoint target, std::vector<Token> tokens,
         std::string_view local_ips = {});

    Metrics run(bool progress = true);

  private:
    [[nodiscard]] double elapsed() const;

    double between(double low, double high);
    bool chance(double probability);
    void observe_connections(double now);
    void connection_established(std::size_t id, double now);
    void schedule(std::size_t id);
    void close(std::size_t id);
    void interest(std::size_t id, bool write_ready, bool add = false);
    void stop(std::size_t id, bool iteration_interrupted);
    void finish_iteration(std::size_t id, double now);
    void idle(User &user, double now);
    void create_or_idle(std::size_t id, double now);
    void tick(std::size_t id, double now);
    void send_request(std::size_t id, Step step, double now);
    void connect(std::size_t id, double now);
    void write(std::size_t id, double now);
    void io(std::size_t id, std::uint32_t events, double now);
    void complete(std::size_t id, bool transport_ok, double now, std::string_view error = {});

    RuntimeConfig config_;
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
    double last_connection_observation_ = 0;
};
} // namespace loadgen
