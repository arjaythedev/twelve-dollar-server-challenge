#ifndef TD_URING_BRIDGE_H
#define TD_URING_BRIDGE_H

#define TD_CAPACITY (8 * 65535)

struct td_net;
struct td_event {
    int kind; /* 1: accepted, 2: input, 3: closed, 4: output drained, 5: checkpoint */
    int id;
    unsigned char *data;
    int len;
    int buffer;
};

struct td_net *td_net_new(int listener, int checkpoint);
int td_net_next(struct td_net *, struct td_event *, int capacity, int timeout_ms);
void td_net_release(struct td_net *, const struct td_event *);
void td_net_send(struct td_net *, int id, const unsigned char *, int len);
void td_net_resume(struct td_net *, int id, int close_after, int notify_drain);
void td_net_abort(struct td_net *, int id);

#endif
