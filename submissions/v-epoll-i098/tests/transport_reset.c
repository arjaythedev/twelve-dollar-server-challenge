#define _GNU_SOURCE
#include <arpa/inet.h>
#include "../uring_bridge.c"

static struct td_event expect_event(struct td_net *net, int kind)
{
    struct td_event event;
    assert(td_net_next(net, &event, 1, 5000) == 1);
    assert(event.kind == kind);
    return event;
}

static int request(struct td_net *net, const struct sockaddr_in *address, int *id)
{
    int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    assert(connect(client, (const struct sockaddr *)address, sizeof(*address)) == 0);
    struct td_event event = expect_event(net, 1);
    *id = event.id;
    td_net_release(net, &event);
    td_net_resume(net, *id, 0, 0);
    assert(send(client, "request", 7, MSG_NOSIGNAL) == 7);
    event = expect_event(net, 2);
    assert(event.id == *id && event.len == 7 && memcmp(event.data, "request", 7) == 0);
    td_net_release(net, &event);
    return client;
}

int main(void)
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 8) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    int checkpoint = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(checkpoint >= 0);
    struct td_net *net = td_net_new(listener, checkpoint);
    int id;
    int client = request(net, &address, &id);
    struct linger reset = { .l_onoff = 1, .l_linger = 0 };
    assert(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(client);
    struct td_event event = expect_event(net, 6);
    assert(event.id == id);
    td_net_release(net, &event);
    td_net_abort(net, id);
    event = expect_event(net, 3);
    assert(event.id == id);
    td_net_release(net, &event);
    puts("PASS reset closes a connection while its application work is deferred");

    client = request(net, &address, &id);
    assert(shutdown(client, SHUT_WR) == 0);
    assert(td_net_next(net, &event, 1, 100) == 0);
    td_net_send(net, id, (const unsigned char *)"OK", 2);
    td_net_resume(net, id, 0, 0);
    event = expect_event(net, 3);
    assert(event.id == id);
    td_net_release(net, &event);
    char response[2];
    assert(recv(client, response, sizeof(response), MSG_WAITALL) == 2);
    assert(memcmp(response, "OK", 2) == 0);
    assert(recv(client, response, sizeof(response), 0) == 0);
    close(client);
    puts("PASS input half-close preserves the pending response");
    close(checkpoint);
    close(listener);
    return 0;
}
