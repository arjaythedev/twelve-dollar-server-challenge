/* Linux loopback mixed-user diagnostic. No dependency beyond libc/libm.
 * cc -O3 -Wall -Wextra connection_load.c -lm -o connection-load
 * connection-load PORT --users 70000 --seconds 300 --ramp 60 --tokens seed/tokens.json
 * The request mix and think times follow bench/load.js; this is not a k6 score.
 * Request latency excludes connect time. End-to-end latency includes it.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { IDLE, CONNECTING, SENDING, RECEIVING };
enum { FEED, POST, DECIDE, LIKE, CREATE };
struct user {
    int fd, state, phase, create, measured, succeeded;
    unsigned generation;
    uint64_t random, post, iteration;
    double begin, request_begin;
    char output[1536];
    size_t output_length, sent, length, capacity, header, body_length;
    char *input;
    int status, closing;
};
struct timer { double due; int user; unsigned generation; };
static struct user *users;
static struct timer *heap;
static size_t heap_length, heap_capacity;
static double *latencies, *end_to_end;
static size_t samples, sample_capacity;
static char **tokens;
static size_t token_count;
static int poller, port, count = 512, offset;
static double hold = 300000, ramp = 60000, timeout = 60000, started, hold_start, end;
static uint64_t requests, errors, connected, retired, successful_users;
static uint64_t created_posts, inserted_likes;
static int open_connections, peak_connections, pending;
static double loop_delay;

static void fail(const char *message) { perror(message); exit(2); }
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double random_value(struct user *u) {
    uint64_t n = u->random;
    n ^= n >> 12; n ^= n << 25; n ^= n >> 27;
    u->random = n;
    return ((n * UINT64_C(2685821657736338717)) >> 11) * (1.0 / 9007199254740992.0);
}
static double between(struct user *u, double lo, double hi) {
    return lo + random_value(u) * (hi - lo);
}
static void schedule(int index, double due) {
    if (heap_length == heap_capacity) {
        heap_capacity = heap_capacity ? heap_capacity * 2 : 4096;
        heap = realloc(heap, heap_capacity * sizeof(*heap));
        if (!heap) fail("timer allocation");
    }
    struct timer item = {due, index, users[index].generation};
    size_t at = heap_length++;
    while (at && heap[(at - 1) / 2].due > due) {
        heap[at] = heap[(at - 1) / 2]; at = (at - 1) / 2;
    }
    heap[at] = item;
}
static struct timer pop_timer(void) {
    struct timer result = heap[0], item = heap[--heap_length];
    if (heap_length) {
        size_t at = 0;
        while (at * 2 + 1 < heap_length) {
            size_t child = at * 2 + 1;
            if (child + 1 < heap_length && heap[child + 1].due < heap[child].due) ++child;
            if (heap[child].due >= item.due) break;
            heap[at] = heap[child]; at = child;
        }
        heap[at] = item;
    }
    return result;
}
static void watch(int index, int add) {
    struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP, .data.u32 = (unsigned)index};
    if (users[index].state == CONNECTING || users[index].state == SENDING)
        event.events |= EPOLLOUT;
    if (epoll_ctl(poller, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, users[index].fd, &event))
        fail("epoll_ctl");
}
static void disconnect(struct user *u) {
    if (u->fd >= 0) {
        epoll_ctl(poller, EPOLL_CTL_DEL, u->fd, NULL);
        close(u->fd); u->fd = -1; --open_connections;
    }
}
static void sample(struct user *u, double finished, int ok) {
    if (ok && !u->succeeded) { u->succeeded = 1; ++successful_users; }
    if (ok && u->phase == CREATE) ++created_posts;
    if (ok && u->phase == LIKE && u->status == 201) ++inserted_likes;
    if (!u->measured) return;
    if (samples == sample_capacity) {
        sample_capacity = sample_capacity ? sample_capacity * 2 : 65536;
        latencies = realloc(latencies, sample_capacity * sizeof(*latencies));
        end_to_end = realloc(end_to_end, sample_capacity * sizeof(*end_to_end));
        if (!latencies || !end_to_end) fail("sample allocation");
    }
    latencies[samples] = u->request_begin ? finished - u->request_begin : 0;
    end_to_end[samples++] = finished - u->begin;
    ++requests; if (!ok) ++errors;
}
static void begin_request(int index);
static void finished(int index, int ok) {
    struct user *u = &users[index];
    double t = now();
    if (ok && u->phase == FEED) {
        uint64_t ids[20];
        int found = 0;
        char *at = u->input + u->header;
        while (found < 20 && (at = strstr(at, "\"id\":"))) {
            at += 5;
            ids[found++] = strtoull(at, NULL, 10);
        }
        if (!found) ok = 0;
        else u->post = ids[(int)(random_value(u) * found)];
    }
    sample(u, t, ok);
    --pending; ++u->generation;
    if (!ok) disconnect(u);
    else if (u->closing) ++retired;
    /* Read the server FIN while idle, instead of racing it with an active close. */
    free(u->input); u->input = NULL; u->length = u->capacity = 0;
    u->state = IDLE;
    if (u->fd >= 0) watch(index, 0);
    if (t >= end) return;
    if (u->phase == FEED) {
        u->phase = ok ? POST : FEED;
        schedule(index, t + between(u, 3000, 7000));
    } else if (u->phase == POST) {
        u->phase = DECIDE;
        schedule(index, t + between(u, 3000, 8000));
    } else if (u->phase == LIKE && u->create) {
        u->phase = CREATE;
        begin_request(index);
    } else {
        ++u->iteration; u->phase = FEED;
        schedule(index, t + between(u, 5000, 15000));
    }
}
static void send_request(int index) {
    struct user *u = &users[index];
    while (u->sent < u->output_length) {
        ssize_t n = send(u->fd, u->output + u->sent, u->output_length - u->sent, MSG_NOSIGNAL);
        if (n > 0) u->sent += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { watch(index, 0); return; }
        else { finished(index, 0); return; }
    }
    u->state = RECEIVING;
    watch(index, 0);
}
static void begin_request(int index) {
    struct user *u = &users[index];
    if (now() >= end) return;
    if (u->closing) disconnect(u);
    if (u->phase == DECIDE) {
        int like = random_value(u) < .15;
        u->create = random_value(u) < .02;
        if (!like && !u->create) {
            ++u->iteration; u->phase = FEED;
            schedule(index, now() + between(u, 5000, 15000)); return;
        }
        u->phase = like ? LIKE : CREATE;
    }
    char path[96], body[192] = "";
    const char *method = u->phase == LIKE || u->phase == CREATE ? "POST" : "GET";
    if (u->phase == FEED) strcpy(path, "/feed");
    else if (u->phase == CREATE) {
        strcpy(path, "/posts");
        snprintf(body, sizeof(body), "{\"body\":\"connection check VU %d says hi at %.0f iteration %llu\"}",
                 index + offset, now(), (unsigned long long)u->iteration);
    } else snprintf(path, sizeof(path), "/posts/%llu%s", (unsigned long long)u->post, u->phase == LIKE ? "/like" : "");
    int n;
    if (!strcmp(method, "GET"))
        n = snprintf(u->output, sizeof(u->output), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", path);
    else
        n = snprintf(u->output, sizeof(u->output),
            "POST %s HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
            path, tokens[((size_t)index + offset) % token_count], strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof(u->output)) fail("request too large");
    u->output_length = (size_t)n; u->sent = 0;
    u->begin = now(); u->request_begin = 0; u->measured = u->begin >= hold_start;
    u->header = u->body_length = 0; u->status = u->closing = 0;
    ++pending; ++u->generation;
    schedule(index, u->begin + timeout);
    if (u->fd < 0) {
        u->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (u->fd < 0) { finished(index, 0); return; }
        ++open_connections; ++connected;
        if (open_connections > peak_connections) peak_connections = open_connections;
        int enabled = 1;
        if (setsockopt(u->fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &enabled, sizeof(enabled)))
            fail("IP_BIND_ADDRESS_NO_PORT");
        unsigned alias = ((unsigned)index + (unsigned)offset) / 2000;
        struct sockaddr_in source = {.sin_family = AF_INET};
        source.sin_addr.s_addr = htonl((127U << 24) | ((1 + alias / 250) << 8) | (1 + alias % 250));
        if (bind(u->fd, (struct sockaddr *)&source, sizeof(source))) { finished(index, 0); return; }
        struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port),
                                     .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        u->state = CONNECTING;
        int result = connect(u->fd, (struct sockaddr *)&target, sizeof(target));
        if (result && errno != EINPROGRESS) { finished(index, 0); return; }
        watch(index, 1);
        if (result) return;
    }
    u->state = SENDING; u->request_begin = now();
    send_request(index);
}
static void receive_response(int index) {
    struct user *u = &users[index];
    for (;;) {
        if (u->capacity - u->length < 4096) {
            size_t capacity = u->capacity ? u->capacity * 2 : 8192;
            if (capacity > 4 * 1024 * 1024) { finished(index, 0); return; }
            u->input = realloc(u->input, capacity + 1);
            if (!u->input) fail("response allocation");
            u->capacity = capacity;
        }
        ssize_t n = recv(u->fd, u->input + u->length, u->capacity - u->length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { finished(index, 0); return; }
        u->length += (size_t)n; u->input[u->length] = 0;
        if (!u->header) {
            char *boundary = strstr(u->input, "\r\n\r\n");
            if (!boundary) continue;
            u->header = (size_t)(boundary - u->input) + 4;
            *boundary = 0;
            char *length = strcasestr(u->input, "\r\nContent-Length:");
            if (!length || sscanf(u->input, "HTTP/1.1 %d", &u->status) != 1) { finished(index, 0); return; }
            u->body_length = strtoull(length + strlen("\r\nContent-Length:"), NULL, 10);
            u->closing = strcasestr(u->input, "\r\nConnection: close") != NULL;
        }
        if (u->length >= u->header + u->body_length) {
            int ok = u->phase == CREATE ? u->status == 201 :
                     u->phase == LIKE ? u->status == 200 || u->status == 201 : u->status == 200;
            finished(index, ok); return;
        }
    }
}
static void read_tokens(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) fail("tokens");
    if (fseek(file, 0, SEEK_END)) fail("tokens seek");
    long size = ftell(file);
    if (size <= 0) fail("empty tokens");
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) fail("tokens read");
    fclose(file); text[size] = 0;
    char *at = text;
    while ((at = strstr(at, "\"token\""))) {
        at = strchr(at + 7, ':');
        if (!at) fail("tokens format");
        at = strchr(at + 1, '"');
        if (!at) fail("tokens format");
        char *last = strchr(++at, '"');
        if (!last) fail("tokens format");
        tokens = realloc(tokens, (token_count + 1) * sizeof(*tokens));
        if (!tokens) fail("token allocation");
        tokens[token_count] = strndup(at, (size_t)(last - at));
        if (!tokens[token_count++]) fail("token allocation");
        at = last + 1;
    }
    free(text);
    if (!token_count) fail("no tokens");
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double percentile(double *values, double q) {
    return samples ? values[(size_t)((samples - 1) * q)] : 0;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: connection-load PORT [--users N --seconds N --ramp N --timeout N --offset N --tokens FILE]\n"); return 2; }
    port = atoi(argv[1]);
    const char *filename = "seed/tokens.json";
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--users")) count = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) hold = atof(argv[i + 1]) * 1000;
        else if (!strcmp(argv[i], "--ramp")) ramp = atof(argv[i + 1]) * 1000;
        else if (!strcmp(argv[i], "--timeout")) timeout = atof(argv[i + 1]) * 1000;
        else if (!strcmp(argv[i], "--offset")) offset = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--tokens")) filename = argv[i + 1];
        else return 2;
    }
    if (count < 1 || count > 1000000 || port < 1 || port > 65535 || hold <= 0 || ramp < 0 || timeout <= 0 || offset < 0) return 2;
    read_tokens(filename);
    users = calloc((size_t)count, sizeof(*users));
    poller = epoll_create1(EPOLL_CLOEXEC);
    if (!users || poller < 0) fail("initialization");
    started = now(); hold_start = started + ramp; end = hold_start + hold;
    for (int i = 0; i < count; ++i) {
        users[i].fd = -1;
        users[i].random = UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)i + offset + 1);
        schedule(i, started + ramp * i / count + between(&users[i], 0, 5000));
    }
    struct epoll_event events[1024];
    double last_report = started, last_tick = now();
    while (now() < end || pending) {
        double t = now();
        if (t >= hold_start && t - last_tick > loop_delay) loop_delay = t - last_tick;
        last_tick = t;
        while (heap_length && heap[0].due <= t) {
            struct timer item = pop_timer();
            struct user *u = &users[item.user];
            if (item.generation != u->generation) continue;
            if (u->state == IDLE) { if (t < end) begin_request(item.user); }
            else finished(item.user, 0);
        }
        int n = epoll_wait(poller, events, 1024, 10);
        if (n < 0 && errno != EINTR) fail("epoll_wait");
        for (int i = 0; i < n; ++i) {
            int index = (int)events[i].data.u32;
            struct user *u = &users[index];
            if (u->fd < 0) continue;
            if (u->state == IDLE) { disconnect(u); continue; }
            if (u->state != CONNECTING && (events[i].events & EPOLLERR)) {
                int error = 0; socklen_t length = sizeof(error);
                if (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
                    finished(index, 0); continue;
                }
            }
            if (u->state == CONNECTING) {
                int error = 0; socklen_t length = sizeof(error);
                if (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) { finished(index, 0); continue; }
                u->state = SENDING; u->request_begin = now();
            }
            if (u->state == SENDING) send_request(index);
            if (u->state == RECEIVING && (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)))
                receive_response(index);
        }
        if (t - last_report >= 30000) {
            printf("{\"progress_seconds\":%.0f,\"requests\":%llu,\"errors\":%llu,\"connections\":%d,\"connects\":%llu,\"retired_connections\":%llu}\n",
                   (t - started) / 1000, (unsigned long long)requests, (unsigned long long)errors, open_connections,
                   (unsigned long long)connected, (unsigned long long)retired);
            fflush(stdout); last_report = t;
        }
    }
    for (int i = 0; i < count; ++i) disconnect(&users[i]);
    qsort(latencies, samples, sizeof(*latencies), compare);
    qsort(end_to_end, samples, sizeof(*end_to_end), compare);
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    double elapsed = (now() - started) / 1000, error_rate = errors / (double)(requests ? requests : 1);
    int pass = samples && successful_users == (uint64_t)count && error_rate < .01 &&
               percentile(latencies, .95) < 500 && percentile(latencies, .99) < 1000 && loop_delay < 1000;
    printf("{\"diagnostic\":\"C epoll mixed user loop; not an official k6 score\",\"configuration\":{\"users\":%d,\"offset\":%d,\"seconds\":%.0f,\"ramp\":%.0f,\"timeout\":%.0f},"
           "\"requests\":%llu,\"errors\":{\"failed_requests\":%llu},\"error_rate\":%.9f,\"successful_users\":%llu,"
           "\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"end_to_end_p99_ms\":%.3f,\"event_loop_delay_ms\":%.3f,"
           "\"peak_connections\":%d,\"connects\":%llu,\"retired_connections\":%llu,\"elapsed_seconds\":%.3f,"
           "\"created_posts\":%llu,\"inserted_likes\":%llu,\"requests_per_second\":%.3f,"
           "\"load_generator_peak_rss_kib\":%ld,\"load_generator_cpu_seconds\":%.3f,\"pass\":%s}\n",
           count, offset, hold / 1000, ramp / 1000, timeout / 1000,
           (unsigned long long)requests, (unsigned long long)errors, error_rate, (unsigned long long)successful_users,
           percentile(latencies, .5), percentile(latencies, .95), percentile(latencies, .99), percentile(end_to_end, .99),
           loop_delay, peak_connections, (unsigned long long)connected, (unsigned long long)retired, elapsed,
           (unsigned long long)created_posts, (unsigned long long)inserted_likes, requests / (hold / 1000),
           usage.ru_maxrss,
           usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6,
           pass ? "true" : "false");
    return pass ? 0 : 1;
}
