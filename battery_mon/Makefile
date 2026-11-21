CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11 -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS ?= -Wl,-z,relro,-z,now
CPPFLAGS ?= -Isrc

TARGET = battery_mon
SRCS = src/main/bm_main.c src/bm_common.c src/bm_binder.c \
       src/bm_txn_cache.c src/m1/root_binder.c src/m2/shell_sysfs_polling.c \
       src/m3/app_broadcastreceiver_method.c src/m4/netlink_uevent.c \
       src/m5/input_battery_listener.c src/m6/battery_properties_registrar.c \
       src/m7/battery_dump.c
HDRS = src/bm_common.h src/bm_binder.h src/bm_txn_cache.h

TEST_BIN = tests/test_parcel
TEST_TXN_BIN = tests/test_txn_cache
CODE_FETCHER = code_fetcher
TXN_HEADER = src/bm_txn_codes.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

$(TEST_BIN): tests/test_parcel.c src/bm_binder.c src/bm_common.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_parcel.c src/bm_binder.c src/bm_common.c

$(TEST_TXN_BIN): tests/test_txn_cache.c src/bm_txn_cache.c src/bm_txn_cache.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_txn_cache.c src/bm_txn_cache.c

test: $(TEST_BIN) $(TEST_TXN_BIN)
	./$(TEST_BIN)
	./$(TEST_TXN_BIN)

$(CODE_FETCHER): tools/code_fetcher.c src/bm_txn_cache.c src/bm_txn_cache.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wl,--export-dynamic -o $@ \
		tools/code_fetcher.c src/bm_txn_cache.c -ldl

# Reads the zygote boot classpath; run as root or with BOOTCLASSPATH set.
code-fetch: $(CODE_FETCHER)
	./$(CODE_FETCHER) > $(TXN_HEADER).tmp && mv $(TXN_HEADER).tmp $(TXN_HEADER)

clean:
	rm -f $(TARGET) $(TEST_BIN) $(TEST_TXN_BIN) $(CODE_FETCHER) \
		tools/code_fetcher $(TXN_HEADER).tmp

.PHONY: all test clean code-fetch
