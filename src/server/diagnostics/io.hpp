#pragma once

#include "../telemetry/io_snapshot.hpp"

#include <cstddef>
#include <cstdint>

namespace tds::net
{
class ConnectionStore;
class Reactor;
class Ring;
enum class Event;
struct Completion;
struct Connection;
} // namespace tds::net

namespace tds::telemetry
{

class IoTelemetry
{
  public:
    IoTelemetry() = default;
    IoTelemetry(const IoTelemetry &) = delete;
    IoTelemetry &operator=(const IoTelemetry &) = delete;

    class Run
    {
      public:
#if TDS_TELEMETRY_LEVEL >= 1
        Run(IoTelemetry &owner, net::Reactor &reactor) noexcept;
        ~Run();
#else
        Run(IoTelemetry &, net::Reactor &) noexcept
        {
        }

        ~Run()
        {
        }
#endif
        Run(const Run &) = delete;
        Run &operator=(const Run &) = delete;

      private:
#if TDS_TELEMETRY_LEVEL >= 1
        IoTelemetry &owner_;
        net::Reactor &reactor_;
#endif
    };

    class Handler
    {
      public:
#if TDS_TELEMETRY_LEVEL == 2
        Handler(IoTelemetry &owner, net::Connection &connection) noexcept;
        ~Handler();
        void finish() noexcept;
#else
        Handler(IoTelemetry &, net::Connection &) noexcept
        {
        }

        ~Handler()
        {
        }

        void finish() noexcept
        {
        }
#endif
        Handler(const Handler &) = delete;
        Handler &operator=(const Handler &) = delete;

      private:
#if TDS_TELEMETRY_LEVEL == 2
        IoTelemetry &owner_;
        std::uint64_t started_ns_ = 0;
        bool active_ = false;
#endif
    };

    class WriteBatch
    {
      public:
#if TDS_TELEMETRY_LEVEL == 2
        WriteBatch(IoTelemetry &owner, std::size_t count) noexcept;
        ~WriteBatch();
#else
        WriteBatch(IoTelemetry &, std::size_t) noexcept
        {
        }

        ~WriteBatch()
        {
        }
#endif
        WriteBatch(const WriteBatch &) = delete;
        WriteBatch &operator=(const WriteBatch &) = delete;

      private:
#if TDS_TELEMETRY_LEVEL == 2
        IoTelemetry &owner_;
        std::uint64_t started_ns_;
#endif
    };

    class Receive
    {
      public:
#if TDS_TELEMETRY_LEVEL == 2
        Receive() noexcept;
        void attach(net::Connection &connection) const noexcept;
#else
        Receive() noexcept = default;

        void attach(net::Connection &) const noexcept
        {
        }
#endif

      private:
#if TDS_TELEMETRY_LEVEL == 2
        std::uint64_t serviced_ns_;
#endif
    };

    Run run(net::Reactor &reactor) noexcept
    {
        return Run(*this, reactor);
    }

    Handler handler(net::Connection &connection) noexcept
    {
        return Handler(*this, connection);
    }

    WriteBatch write_batch(std::size_t count) noexcept
    {
        return WriteBatch(*this, count);
    }

    Receive receive() noexcept
    {
        return Receive{};
    }

#if TDS_TELEMETRY_LEVEL == 2
    void report(const net::Reactor &reactor, bool force = false) noexcept;
    static void send_prepared(net::Connection &connection) noexcept;
    static void send_completed(const net::Completion &completion) noexcept;
    static void receive_deferred(net::Connection &connection) noexcept;

    const IoSnapshot &totals() const noexcept
    {
        return totals_;
    }

    const History<IoSnapshot> &history() const noexcept
    {
        return history_;
    }
#else
    void report(const net::Reactor &, bool = false) noexcept
    {
    }

    static void send_prepared(net::Connection &) noexcept
    {
    }

    static void send_completed(const net::Completion &) noexcept
    {
    }

    static void receive_deferred(net::Connection &) noexcept
    {
    }
#endif

#if TDS_TELEMETRY_LEVEL >= 1
    static void drain_started(net::Ring &owner) noexcept;
    static void drain_finished(net::Ring &owner, unsigned drained) noexcept;
    static void receive_prepared(net::Connection &connection) noexcept;
    static void io_serviced(net::Connection &connection, net::Event event, int result,
                            net::Completion &completion) noexcept;
    static void store_created(net::ConnectionStore &owner) noexcept;
    static void store_retired(net::ConnectionStore &owner) noexcept;
    static void exchange_allocated(net::Ring &owner) noexcept;
    static void exchange_reused(net::Ring &owner) noexcept;
#else
    static void drain_started(net::Ring &) noexcept
    {
    }

    static void drain_finished(net::Ring &, unsigned) noexcept
    {
    }

    static void receive_prepared(net::Connection &) noexcept
    {
    }

    static void io_serviced(net::Connection &, net::Event, int, net::Completion &) noexcept
    {
    }

    static void store_created(net::ConnectionStore &) noexcept
    {
    }

    static void store_retired(net::ConnectionStore &) noexcept
    {
    }

    static void exchange_allocated(net::Ring &) noexcept
    {
    }

    static void exchange_reused(net::Ring &) noexcept
    {
    }
#endif

  private:
#if TDS_TELEMETRY_LEVEL >= 1
    void finalize(const net::Reactor &reactor) noexcept;
    static RingSnapshot snapshot_ring(net::Ring &owner, std::size_t index,
                                      std::uint64_t now) noexcept;
#endif
#if TDS_TELEMETRY_LEVEL == 2
    void start() noexcept;

    IoTimings interval_;
    IoSnapshot totals_;
    History<IoSnapshot> history_;
#endif
};

} // namespace tds::telemetry
