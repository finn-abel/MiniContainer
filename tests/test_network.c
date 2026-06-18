#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "network.h"

static void test_pick_ip_returns_first_host_when_only_gateway_used(void) {
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    char ip[MINICTL_MAX_ID_SIZE];

    memset(used, 0, sizeof(used));
    used[MINICTL_NETWORK_GATEWAY_HOST] = true;

    assert(network_pick_ip(used, sizeof(used) / sizeof(used[0]), ip, sizeof(ip)) == 0);
    assert(strcmp(ip, "10.0.0.2") == 0);
}

static void test_pick_ip_skips_used_and_picks_lowest_free(void) {
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    char ip[MINICTL_MAX_ID_SIZE];

    memset(used, 0, sizeof(used));
    used[MINICTL_NETWORK_GATEWAY_HOST] = true;
    used[2] = true;
    used[3] = true;

    assert(network_pick_ip(used, sizeof(used) / sizeof(used[0]), ip, sizeof(ip)) == 0);
    assert(strcmp(ip, "10.0.0.4") == 0);
}

static void test_pick_ip_fills_gaps_lowest_first(void) {
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    char ip[MINICTL_MAX_ID_SIZE];

    memset(used, 0, sizeof(used));
    used[MINICTL_NETWORK_GATEWAY_HOST] = true;
    used[2] = true;
    used[4] = true;

    assert(network_pick_ip(used, sizeof(used) / sizeof(used[0]), ip, sizeof(ip)) == 0);
    assert(strcmp(ip, "10.0.0.3") == 0);
}

static void test_pick_ip_never_returns_gateway(void) {
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    char ip[MINICTL_MAX_ID_SIZE];
    int host;

    /* Mark every assignable host used so only the gateway octet remains free. */
    memset(used, 0, sizeof(used));
    for (host = MINICTL_NETWORK_FIRST_HOST; host <= MINICTL_NETWORK_LAST_HOST; host++) {
        used[host] = true;
    }

    errno = 0;
    assert(network_pick_ip(used, sizeof(used) / sizeof(used[0]), ip, sizeof(ip)) == -1);
    assert(errno == EADDRNOTAVAIL);
}

static void test_pick_ip_rejects_null_arguments(void) {
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    char ip[MINICTL_MAX_ID_SIZE];

    memset(used, 0, sizeof(used));

    errno = 0;
    assert(network_pick_ip(NULL, sizeof(used) / sizeof(used[0]), ip, sizeof(ip)) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(network_pick_ip(used, sizeof(used) / sizeof(used[0]), NULL, sizeof(ip)) == -1);
    assert(errno == EINVAL);
}

static void test_veth_names_use_expected_prefixes(void) {
    char host_name[64];
    char peer_name[64];

    assert(network_veth_host_name("0a1b2c3d", host_name, sizeof(host_name)) == 0);
    assert(strcmp(host_name, "veth0a1b2c3d") == 0);

    assert(network_veth_peer_name("0a1b2c3d", peer_name, sizeof(peer_name)) == 0);
    assert(strcmp(peer_name, "vethc0a1b2c3d") == 0);
}

static void test_veth_names_reject_overlong_id(void) {
    char name[64];

    /* An id this long pushes the generated name past the kernel name limit. */
    errno = 0;
    assert(network_veth_peer_name("abcdefghijklmnop", name, sizeof(name)) == -1);
    assert(errno == ENAMETOOLONG);
}

static void test_veth_names_reject_empty_id(void) {
    char name[64];

    errno = 0;
    assert(network_veth_host_name("", name, sizeof(name)) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_pick_ip_returns_first_host_when_only_gateway_used();
    test_pick_ip_skips_used_and_picks_lowest_free();
    test_pick_ip_fills_gaps_lowest_first();
    test_pick_ip_never_returns_gateway();
    test_pick_ip_rejects_null_arguments();
    test_veth_names_use_expected_prefixes();
    test_veth_names_reject_overlong_id();
    test_veth_names_reject_empty_id();

    printf("All network tests passed.\n");
    return 0;
}
