CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -D_GNU_SOURCE
INCLUDES = -Iinclude

TARGET = minictl

# Add normal project source files here.
SRC = src/main.c src/cli.c src/cgroups.c src/container.c src/logging.c src/namespaces.c src/network.c src/process.c src/proxy.c src/rootfs.c src/state.c src/util.c

# Add test source files here.
TEST_SRC = tests/test_util.c tests/test_cli.c tests/test_cli_errors.c tests/test_state.c tests/test_container.c tests/test_process.c tests/test_namespaces.c tests/test_network.c tests/test_proxy.c tests/test_rootfs.c tests/test_logging.c tests/test_cgroups_parse.c

OBJ = $(SRC:.c=.o)

# Source files needed for tests should not include src/main.c,
# because each test file has its own main function.
TEST_SUPPORT_SRC = $(filter-out src/main.c, $(SRC))
TEST_SUPPORT_OBJ = $(TEST_SUPPORT_SRC:.c=.o)

TEST_TARGETS = $(TEST_SRC:tests/%.c=%)
TEST_OBJ = $(TEST_SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%: tests/%.o $(TEST_SUPPORT_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test: all $(TEST_TARGETS)
	@set -e; \
	for test in $(TEST_TARGETS); do \
		echo "Running $$test..."; \
		./$$test; \
		echo ""; \
	done; \
	rm -f $(TEST_TARGETS) $(TEST_OBJ); \
	rm -rf *.dSYM tests/*.dSYM

clean:
	rm -f $(OBJ) $(TEST_SUPPORT_OBJ) $(TEST_OBJ) $(TARGET) $(TEST_TARGETS)
	rm -rf *.dSYM tests/*.dSYM

run: $(TARGET)
	./$(TARGET)

integration: $(TARGET)
	ROOTFS="$(ROOTFS)" MINICTL="./$(TARGET)" sh tests/integration.sh

.PHONY: all clean run test integration
