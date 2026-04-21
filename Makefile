CC      ?= gcc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
OPT     ?= -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) -Iinclude -pthread
LDFLAGS ?= -pthread

BIN     := tick-relay
OBJDIR  := build

SRC       := $(wildcard src/*.c)
OBJ       := $(SRC:src/%.c=$(OBJDIR)/%.o)
NONMAIN   := $(filter-out $(OBJDIR)/main.o,$(OBJ))

TEST_BINS := $(OBJDIR)/test_ring $(OBJDIR)/test_histogram

.PHONY: all clean test asan release run-bench

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/test_ring: tests/test_ring.c src/ring.c src/feed.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_ring.c src/ring.c src/feed.c $(LDFLAGS)

$(OBJDIR)/test_histogram: tests/test_histogram.c src/histogram.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ tests/test_histogram.c src/histogram.c $(LDFLAGS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "-- running $$t"; ./$$t; done

asan:
	$(MAKE) clean
	$(MAKE) all test \
		CFLAGS="$(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -pthread" \
		LDFLAGS="-pthread -fsanitize=address,undefined"

release:
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(CSTD) $(WARN) -O3 -march=native -flto -DNDEBUG -Iinclude -pthread" \
		LDFLAGS="-pthread -flto"

run-bench: release
	./scripts/run_bench.sh

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(OBJ:.o=.d)
