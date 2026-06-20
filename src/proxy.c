#include "proxy.h"

#include "config.h"
#include "process.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * One read/write chunk for the connection pump.
 * Keep it large enough for throughput but small enough for stack allocation.
 */
#define PROXY_BUFFER_SIZE 65536
#define PROXY_LISTEN_BACKLOG 16

int proxy_parse_publish(const char *value, PublishSpec *out)
{
    const char *colon;
    char *end = NULL;
    long host;
    long container;

    if (value == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The mapping is exactly HOST:CONTAINER. Require a separator with non-empty
     * sides so values like "8080", "8080:", and ":80" are rejected clearly.
     */
    colon = strchr(value, ':');
    if (colon == NULL || colon == value || *(colon + 1) == '\0') {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    host = strtol(value, &end, 10);
    /*
     * strtol must stop exactly at the colon. Accepting a partial parse would
     * allow malformed host ports like "8080abc:80" to reach runtime setup.
     */
    if (errno != 0 || end != colon || host < MINICTL_MIN_PORT || host > MINICTL_MAX_PORT) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    container = strtol(colon + 1, &end, 10);
    /*
     * The container side must consume the rest of the string.
     * Extra separators are rejected here rather than treated as ignored text.
     */
    if (errno != 0 || *end != '\0' || end == colon + 1 || container < MINICTL_MIN_PORT || container > MINICTL_MAX_PORT) {
        errno = EINVAL;
        return -1;
    }

    out->host_port = (int)host;
    out->container_port = (int)container;
    return 0;
}

int proxy_publishes_to_string(const PublishSpec *specs, size_t count, char *out, size_t out_size)
{
    size_t used = 0;
    size_t i;

    if (out == NULL || out_size == 0 || (count > 0 && specs == NULL)) {
        errno = EINVAL;
        return -1;
    }

    out[0] = '\0';
    for (i = 0; i < count; i++) {
        int written = snprintf(out + used, out_size - used, "%s%d:%d",
                               i == 0 ? "" : ",", specs[i].host_port, specs[i].container_port);

        /*
         * Metadata stores all publishes in one compact string.
         * Fail instead of truncating so inspect/state never show partial rules.
         */
        if (written < 0 || (size_t)written >= out_size - used) {
            errno = ENAMETOOLONG;
            return -1;
        }
        used += (size_t)written;
    }

    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    /*
     * Proxy traffic can hit short writes on sockets.
     * Keep writing until the full chunk has crossed or a real error occurs.
     */
    while (sent < len) {
        ssize_t written = write(fd, buf + sent, len - sent);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)written;
    }

    return 0;
}

static int create_listener(int port)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    /*
     * SO_REUSEADDR keeps quick container restarts from hitting TIME_WAIT binds.
     * Bind failures still surface normally when another listener owns the port.
     */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, PROXY_LISTEN_BACKLOG) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_backend(const char *ip, int port)
{
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    /*
     * The backend address is the container IP assigned by the network layer.
     * Keep this IPv4-only for the v1 proxy, matching the bridge implementation.
     */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void pump_connection(int client_fd, int backend_fd)
{
    struct pollfd fds[2];
    char buffer[PROXY_BUFFER_SIZE];
    bool client_open = true;
    bool backend_open = true;

    /*
     * Forward bytes in both directions until each half closes. Reaching EOF on
     * one side is propagated with a half-close (shutdown) so the peer can finish
     * draining before the connection is fully torn down.
     */
    fds[0].fd = client_fd;
    fds[1].fd = backend_fd;
    while (client_open || backend_open) {
        fds[0].events = client_open ? POLLIN : 0;
        fds[1].events = backend_open ? POLLIN : 0;

        /*
         * poll lets either side drive progress. This avoids blocking forever on
         * one direction while the other side has data waiting to be forwarded.
         */
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (client_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(client_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                /*
                 * Half-close instead of full-close so protocols that send a
                 * final response after request EOF still have room to finish.
                 */
                client_open = false;
                shutdown(backend_fd, SHUT_WR);
            } else if (write_all(backend_fd, buffer, (size_t)n) != 0) {
                break;
            }
        }

        if (backend_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(backend_fd, buffer, sizeof(buffer));
            if (n <= 0) {
                /*
                 * Mirror the same half-close behavior in the reverse direction.
                 * The outer loop exits only after both directions are closed.
                 */
                backend_open = false;
                shutdown(client_fd, SHUT_WR);
            } else if (write_all(client_fd, buffer, (size_t)n) != 0) {
                break;
            }
        }
    }

    close(client_fd);
    close(backend_fd);
}

static void handle_connection(int client_fd, const char *ip, int container_port)
{
    int backend_fd = connect_backend(ip, container_port);

    /*
     * A backend that is not listening yet (the container service has not started)
     * just drops the connection; the client sees a closed socket and can retry.
     */
    if (backend_fd < 0) {
        close(client_fd);
        return;
    }

    pump_connection(client_fd, backend_fd);
}

static void run_accept_loop(const int *listen_fds, const PublishSpec *specs, size_t count, const char *ip)
{
    struct pollfd fds[MINICTL_MAX_PUBLISH];
    size_t i;

    for (i = 0; i < count; i++) {
        fds[i].fd = listen_fds[i];
        fds[i].events = POLLIN;
    }

    for (;;) {
        if (poll(fds, (nfds_t)count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            /*
             * A non-interrupt poll failure means the listener set is no longer
             * trustworthy. Returning lets the proxy process exit cleanly.
             */
            return;
        }

        for (i = 0; i < count; i++) {
            int client_fd;
            pid_t child;

            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }

            client_fd = accept(listen_fds[i], NULL, NULL);
            if (client_fd < 0) {
                /*
                 * Transient accept failures should not bring down every
                 * published port. The next poll iteration can accept again.
                 */
                continue;
            }

            /*
             * Fork per connection: SIGCHLD is ignored in the proxy, so these
             * children are auto-reaped, and the whole process group is torn down
             * together when the container exits.
             */
            child = fork();
            if (child < 0) {
                /*
                 * Could not spawn a connection handler (likely resource limits).
                 * Surface the failure and drop just this connection rather than
                 * silently closing the client socket.
                 */
                minictl_perror("publish");
                close(client_fd);
                continue;
            }
            if (child == 0) {
                size_t j;
                /*
                 * Connection children only need their accepted socket and the
                 * backend socket they open. Closing listeners prevents leaked
                 * references from keeping ports bound after proxy shutdown.
                 */
                for (j = 0; j < count; j++) {
                    close(listen_fds[j]);
                }
                handle_connection(client_fd, ip, specs[i].container_port);
                _exit(0);
            }

            /*
             * The parent proxy keeps listening and does not own the accepted
             * client socket after the connection child is forked.
             */
            close(client_fd);
        }
    }
}

#define PROXY_FD_CLOSE_FALLBACK 4096

static void close_inherited_fds(int keep_fd)
{
    struct rlimit limit;
    long max_fd = PROXY_FD_CLOSE_FALLBACK;
    int fd;

    /*
     * The proxy must not keep the run/monitor's pipes open (especially the
     * container log pipe), or the parent's reads would never see EOF. Close every
     * inherited descriptor except the readiness pipe used to report bind status.
     *
     * Prefer the actual soft descriptor limit so we do not leave high fds open
     * when RLIMIT_NOFILE exceeds the fallback. RLIM_INFINITY would loop too far,
     * so cap it to the fallback in that case.
     */
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur <= (rlim_t)PROXY_FD_CLOSE_FALLBACK * 256) {
        max_fd = (long)limit.rlim_cur;
    }
    if (max_fd < PROXY_FD_CLOSE_FALLBACK) {
        max_fd = PROXY_FD_CLOSE_FALLBACK;
    }
    for (fd = 3; fd < max_fd; fd++) {
        if (fd != keep_fd) {
            close(fd);
        }
    }
}

static void proxy_child_main(int ready_fd, const char *ip, const PublishSpec *specs, size_t count)
{
    int listen_fds[MINICTL_MAX_PUBLISH];
    size_t i;
    char status;

    /*
     * Become a process group leader so the owner can tear the proxy and all of
     * its per-connection children down with a single group signal.
     */
    setpgid(0, 0);
    signal(SIGCHLD, SIG_IGN);
    close_inherited_fds(ready_fd);

    for (i = 0; i < count; i++) {
        listen_fds[i] = create_listener(specs[i].host_port);
        if (listen_fds[i] < 0) {
            size_t j;

            minictl_perror("publish");
            /*
             * Any failed bind makes the whole publish set invalid.
             * Close listeners already created before reporting startup failure.
             */
            for (j = 0; j < i; j++) {
                close(listen_fds[j]);
            }
            status = 'F';
            (void)write_all(ready_fd, &status, 1);
            close(ready_fd);
            _exit(1);
        }
    }

    status = 'S';
    /*
     * The parent waits for this byte before returning from proxy_start.
     * That makes published ports ready before container_run reports success.
     */
    if (write_all(ready_fd, &status, 1) != 0) {
        _exit(1);
    }
    close(ready_fd);

    run_accept_loop(listen_fds, specs, count, ip);
    _exit(0);
}

int proxy_start(const char *container_ip, const PublishSpec *specs, size_t count, pid_t *proxy_pid)
{
    int ready[2];
    pid_t pid;
    char status = 'F';
    ssize_t got;

    if (container_ip == NULL || container_ip[0] == '\0' || specs == NULL || proxy_pid == NULL ||
        count == 0 || count > MINICTL_MAX_PUBLISH) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(ready) != 0) {
        return -1;
    }

    /*
     * The readiness pipe turns asynchronous bind failures into synchronous
     * proxy_start errors, so callers can clean up container state immediately.
     */
    pid = fork();
    if (pid < 0) {
        int saved_errno = errno;

        close(ready[0]);
        close(ready[1]);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        close(ready[0]);
        proxy_child_main(ready[1], container_ip, specs, count);
        _exit(1);
    }

    close(ready[1]);
    /*
     * Read exactly one readiness byte. EINTR is harmless because the child will
     * either write success/failure or exit and close the pipe.
     */
    do {
        got = read(ready[0], &status, 1);
    } while (got < 0 && errno == EINTR);
    close(ready[0]);

    if (got != 1 || status != 'S') {
        int code;

        /*
         * The child already printed the precise bind failure. Make sure it is
         * gone and reaped before returning so a failed publish leaves no proxy.
         */
        kill(pid, SIGKILL);
        process_wait(pid, &code);
        errno = EADDRINUSE;
        return -1;
    }

    *proxy_pid = pid;
    return 0;
}

void proxy_stop(pid_t proxy_pid)
{
    if (proxy_pid <= 0) {
        return;
    }

    /*
     * Signal the whole process group so in-flight connection children die with
     * the proxy. This path is for non-owning callers (rm), so liveness is polled
     * rather than reaped via waitpid.
     */
    if (kill(-proxy_pid, SIGTERM) != 0 && errno == ESRCH) {
        return;
    }

    if (process_wait_until_dead(proxy_pid, 2000) != 0) {
        kill(-proxy_pid, SIGKILL);
        process_wait_until_dead(proxy_pid, 2000);
    }
}
