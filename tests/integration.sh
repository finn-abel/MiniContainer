#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "minictl integration: must run as root" >&2
    exit 1
fi

if [ "${ROOTFS:-}" = "" ]; then
    echo "minictl integration: ROOTFS must point at a valid rootfs" >&2
    exit 1
fi

MINICTL=${MINICTL:-./minictl}
STATE_DIR=$(mktemp -d /tmp/minictl-integration.XXXXXX)
CONTAINER_ID=""
NET_CONTAINER_ID=""
PUB_CONTAINER_ID=""

cleanup() {
    for id in "$CONTAINER_ID" "$NET_CONTAINER_ID" "$PUB_CONTAINER_ID"; do
        if [ "$id" != "" ]; then
            MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" stop "$id" >/dev/null 2>&1 || true
            MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" rm "$id" >/dev/null 2>&1 || true
        fi
    done
    rm -rf "$STATE_DIR"
}

trap cleanup EXIT INT TERM

run_output=$(
    MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" run \
        --rootfs "$ROOTFS" \
        --detach \
        --name integration \
        --hostname minictl-it \
        -- /bin/sh -c "while true; do echo ready; sleep 60; done"
)

CONTAINER_ID=$(printf "%s\n" "$run_output" | sed -n 's/^container_id: //p')
if [ "$CONTAINER_ID" = "" ]; then
    echo "minictl integration: failed to parse container id" >&2
    printf "%s\n" "$run_output" >&2
    exit 1
fi

sleep 2

hostname_output=$(MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" exec "$CONTAINER_ID" -- /bin/hostname)
if [ "$hostname_output" != "minictl-it" ]; then
    echo "minictl integration: hostname mismatch: $hostname_output" >&2
    exit 1
fi

MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" exec "$CONTAINER_ID" -- /bin/sh -c "test -d /proc/1"
MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" exec "$CONTAINER_ID" -- /bin/sh -c "test -d /bin"

if ! MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" logs "$CONTAINER_ID" | grep -q "ready"; then
    echo "minictl integration: expected log output not found" >&2
    exit 1
fi

MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" stop "$CONTAINER_ID"
MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" rm "$CONTAINER_ID"
CONTAINER_ID=""

# Bridge networking: requires host iproute2 + util-linux. Skip cleanly if absent
# so the core lifecycle test still runs on minimal hosts.
if command -v ip >/dev/null 2>&1 && command -v nsenter >/dev/null 2>&1; then
    net_output=$(
        MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" run \
            --rootfs "$ROOTFS" \
            --detach \
            --name integration-net \
            --network bridge \
            -- /bin/sh -c "while true; do sleep 60; done"
    )

    NET_CONTAINER_ID=$(printf "%s\n" "$net_output" | sed -n 's/^container_id: //p')
    if [ "$NET_CONTAINER_ID" = "" ]; then
        echo "minictl integration: failed to parse bridge container id" >&2
        printf "%s\n" "$net_output" >&2
        exit 1
    fi

    sleep 1

    # eth0 should exist inside the container with the allocated address.
    if ! MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" exec "$NET_CONTAINER_ID" -- /bin/sh -c "ip addr show eth0" | grep -q "10.0.0."; then
        echo "minictl integration: bridge container has no eth0 address" >&2
        exit 1
    fi

    # The container should reach the bridge gateway (no NAT/internet in this step).
    if ! MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" exec "$NET_CONTAINER_ID" -- /bin/sh -c "ping -c 1 -W 2 10.0.0.1"; then
        echo "minictl integration: bridge container cannot reach gateway" >&2
        exit 1
    fi

    MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" stop "$NET_CONTAINER_ID"
    MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" rm "$NET_CONTAINER_ID"
    NET_CONTAINER_ID=""
else
    echo "minictl integration: ip/nsenter not found, skipping bridge network test"
fi

# Port publishing: needs host iproute2/util-linux + curl, and httpd in the rootfs.
# Skip cleanly when any piece is missing so minimal hosts still pass.
if command -v ip >/dev/null 2>&1 && command -v nsenter >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
    pub_output=$(
        MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" run \
            --rootfs "$ROOTFS" \
            --detach \
            --name integration-pub \
            --publish 8080:80 \
            -- /bin/sh -c "mkdir -p /www && echo minictl-ok > /www/index.html && httpd -f -h /www -p 80"
    )

    PUB_CONTAINER_ID=$(printf "%s\n" "$pub_output" | sed -n 's/^container_id: //p')
    if [ "$PUB_CONTAINER_ID" = "" ]; then
        echo "minictl integration: failed to parse published container id" >&2
        printf "%s\n" "$pub_output" >&2
        exit 1
    fi

    sleep 2

    # The container httpd should be reachable on the published host port.
    if ! curl -fs --max-time 5 http://localhost:8080/ | grep -q "minictl-ok"; then
        echo "minictl integration: published port did not serve container httpd" >&2
        exit 1
    fi

    MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" stop "$PUB_CONTAINER_ID"
    MINICTL_STATE_DIR="$STATE_DIR" "$MINICTL" rm "$PUB_CONTAINER_ID"

    # After rm the proxy must be gone and the host port released.
    if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":8080 "; then
        echo "minictl integration: host port 8080 still bound after rm" >&2
        exit 1
    fi
    PUB_CONTAINER_ID=""
else
    echo "minictl integration: ip/nsenter/curl not all present, skipping publish test"
fi

echo "MiniContainer integration tests passed."
