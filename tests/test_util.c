#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

static void test_str_copy(void) {
    char buf[8];

    assert(minictl_str_copy(buf, sizeof(buf), "abc") == 0);
    assert(strcmp(buf, "abc") == 0);

    errno = 0;
    assert(minictl_str_copy(buf, 4, "abcd") == -1);
    assert(errno == ENAMETOOLONG);
    assert(strcmp(buf, "") == 0);
}

static void test_path_join(void) {
    char path[MINICTL_MAX_PATH_SIZE];
    char small[8];

    assert(minictl_path_join(path, sizeof(path), "/tmp", "minictl") == 0);
    assert(strcmp(path, "/tmp/minictl") == 0);

    assert(minictl_path_join(path, sizeof(path), "/tmp/", "minictl") == 0);
    assert(strcmp(path, "/tmp/minictl") == 0);

    errno = 0;
    assert(minictl_path_join(small, sizeof(small), "/tmp", "minictl") == -1);
    assert(errno == ENAMETOOLONG);
    assert(strcmp(small, "") == 0);
}

static void test_mkdir_and_exists(void) {
    char template[] = "/tmp/minictl-test-XXXXXX";
    char nested[MINICTL_MAX_PATH_SIZE];
    char file_path[MINICTL_MAX_PATH_SIZE];
    FILE *file;

    assert(mkdtemp(template) != NULL);
    assert(minictl_path_join(nested, sizeof(nested), template, "a/b") == 0);
    assert(minictl_mkdir_p(nested, 0700) == 0);
    assert(minictl_dir_exists(nested));
    assert(!minictl_file_exists(nested));

    assert(minictl_path_join(file_path, sizeof(file_path), template, "file") == 0);
    file = fopen(file_path, "w");
    assert(file != NULL);
    assert(fclose(file) == 0);
    assert(minictl_file_exists(file_path));
    assert(!minictl_dir_exists(file_path));

    assert(remove(file_path) == 0);
    assert(rmdir(nested) == 0);
    assert(strcpy(nested, template) == nested);
    assert(strcat(nested, "/a") == nested);
    assert(rmdir(nested) == 0);
    assert(rmdir(template) == 0);
}

int main(void) {
    test_str_copy();
    test_path_join();
    test_mkdir_and_exists();

    printf("All utility tests passed.\n");
    return 0;
}
