#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proxy.h"

static void test_parse_valid_mapping(void) {
    PublishSpec spec = {0, 0};

    assert(proxy_parse_publish("8080:80", &spec) == 0);
    assert(spec.host_port == 8080);
    assert(spec.container_port == 80);

    assert(proxy_parse_publish("65535:1", &spec) == 0);
    assert(spec.host_port == 65535);
    assert(spec.container_port == 1);
}

static void test_parse_rejects_bad_mappings(void) {
    PublishSpec spec = {0, 0};
    const char *bad[] = {"8080", "8080:", ":80", "0:80", "80:0", "70000:80", "80:70000", "a:80", "80:b", "8080:80:90"};
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        errno = 0;
        assert(proxy_parse_publish(bad[i], &spec) == -1);
        assert(errno == EINVAL);
    }

    errno = 0;
    assert(proxy_parse_publish(NULL, &spec) == -1);
    assert(errno == EINVAL);
}

static void test_publishes_to_string(void) {
    PublishSpec specs[] = {{8080, 80}, {9090, 90}};
    char out[64];

    assert(proxy_publishes_to_string(specs, 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "") == 0);

    assert(proxy_publishes_to_string(specs, 1, out, sizeof(out)) == 0);
    assert(strcmp(out, "8080:80") == 0);

    assert(proxy_publishes_to_string(specs, 2, out, sizeof(out)) == 0);
    assert(strcmp(out, "8080:80,9090:90") == 0);
}

static void test_publishes_to_string_overflow(void) {
    PublishSpec specs[] = {{8080, 80}, {9090, 90}};
    char out[8];

    errno = 0;
    assert(proxy_publishes_to_string(specs, 2, out, sizeof(out)) == -1);
    assert(errno == ENAMETOOLONG);
}

static int make_loopback_listener(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    assert(fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 8) == 0);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

static int find_free_port(void) {
    int port;
    int fd = make_loopback_listener(&port);

    close(fd);
    return port;
}

static void run_echo_server(int listen_fd) {
    int conn = accept(listen_fd, NULL, NULL);
    char buf[1024];
    ssize_t n;

    if (conn < 0) {
        _exit(1);
    }
    while ((n = read(conn, buf, sizeof(buf))) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(conn, buf + sent, (size_t)(n - sent));
            if (w < 0) {
                _exit(1);
            }
            sent += w;
        }
    }
    close(conn);
    close(listen_fd);
    _exit(0);
}

static void test_forwarding_round_trip(void) {
    int backend_port;
    int backend_fd = make_loopback_listener(&backend_port);
    int host_port;
    PublishSpec spec;
    pid_t echo_pid;
    pid_t proxy_pid = -1;
    int client;
    int status;
    struct sockaddr_in addr;
    struct timeval timeout = {3, 0};
    char reply[16];
    ssize_t got;

    /* Backend echo server stands in for a container service on 127.0.0.1. */
    echo_pid = fork();
    assert(echo_pid >= 0);
    if (echo_pid == 0) {
        run_echo_server(backend_fd);
        _exit(0);
    }
    close(backend_fd);

    /* proxy_start reports success only after binding, so the listener is ready. */
    host_port = find_free_port();
    spec.host_port = host_port;
    spec.container_port = backend_port;
    assert(proxy_start("127.0.0.1", &spec, 1, &proxy_pid) == 0);
    assert(proxy_pid > 0);

    client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)host_port);
    assert(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    assert(write(client, "ping", 4) == 4);
    got = read(client, reply, sizeof(reply));
    assert(got == 4);
    assert(memcmp(reply, "ping", 4) == 0);
    close(client);

    /* Reap directly so the test stays fast; proxy_stop itself is covered live. */
    kill(proxy_pid, SIGKILL);
    waitpid(proxy_pid, &status, 0);
    kill(echo_pid, SIGKILL);
    waitpid(echo_pid, &status, 0);
}

int main(void) {
    test_parse_valid_mapping();
    test_parse_rejects_bad_mappings();
    test_publishes_to_string();
    test_publishes_to_string_overflow();
    test_forwarding_round_trip();

    printf("All proxy tests passed.\n");
    return 0;
}
