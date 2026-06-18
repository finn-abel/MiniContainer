#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "process.h"

static void test_current_process_is_alive(void) {
    assert(process_is_alive(getpid()) == 1);
}

static void test_fake_pid_is_not_alive(void) {
    assert(process_is_alive(-1) == 0);
    assert(process_is_alive(0) == 0);
}

static void test_status_conversion(void) {
    ContainerStatus status;

    assert(process_status_from_pid(getpid(), &status) == 0);
    assert(status == CONTAINER_STATUS_RUNNING);

    assert(process_status_from_pid(-1, &status) == 0);
    assert(status == CONTAINER_STATUS_EXITED);
}

static void test_wait_normal_exit(void) {
    pid_t pid = fork();
    int exit_code = -1;

    assert(pid >= 0);
    if (pid == 0) {
        _exit(42);
    }

    assert(process_wait(pid, &exit_code) == 0);
    assert(exit_code == 42);
}

static void test_wait_signal_exit(void) {
    pid_t pid = fork();
    int exit_code = -1;

    assert(pid >= 0);
    if (pid == 0) {
        pause();
        _exit(0);
    }

    assert(process_send_signal(pid, SIGTERM) == 0);
    assert(process_wait(pid, &exit_code) == 0);
    assert(exit_code == 128 + SIGTERM);
}

static void test_wait_until_dead_for_fake_pid(void) {
    assert(process_wait_until_dead(-1, 0) == -1);

    assert(process_wait_until_dead(999999, 0) == 0);
}

int main(void) {
    test_current_process_is_alive();
    test_fake_pid_is_not_alive();
    test_status_conversion();
    test_wait_normal_exit();
    test_wait_signal_exit();
    test_wait_until_dead_for_fake_pid();

    printf("All process tests passed.\n");
    return 0;
}
