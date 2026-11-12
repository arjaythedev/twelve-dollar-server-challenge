#ifndef TWELVE_BYPASS_LOCAL_H
#define TWELVE_BYPASS_LOCAL_H

/* A shared-address XDP service also accepts Linux TCP traffic routed locally.
 * The link owns the listener. The reactor owns the returned epoll descriptor.
 * Step runs one bounded, nonblocking pass on the same SQLite issuer thread. */
int local_open(int listener);
int local_step(void);
unsigned local_timeout_ms(void);
void local_close(void);

#endif
