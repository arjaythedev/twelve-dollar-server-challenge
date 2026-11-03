#ifndef TDS_WAL_ADVICE_H
#define TDS_WAL_ADVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        TDS_WAL_ADVICE_EVENT_CAPACITY = 16
    };

    typedef struct tds_wal_advice_event
    {
        uint64_t time_ns;
        uint64_t offset;
        uint64_t length;
        int result;
    } tds_wal_advice_event;

    typedef struct tds_wal_advice_snapshot
    {
        uint64_t calls;
        uint64_t errors;
        uint64_t requested_bytes;
        uint64_t event_count;
        uint64_t dropped_events;
        tds_wal_advice_event events[TDS_WAL_ADVICE_EVENT_CAPACITY];
    } tds_wal_advice_snapshot;

    /* Transfer and reset the calling checkpoint thread's accumulated advice data */
    void tds_wal_advice_take(tds_wal_advice_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
