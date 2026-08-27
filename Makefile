DEBUG ?= FALSE

GCC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn
HOST_CC ?= cc
HOST_CFLAGS ?= -Wall -Wextra -std=c99

GCCFLAGS = -Wall -Wextra -marm -std=c99
LDFLAGS = -Wl,--nspireio,--gc-sections
ZEHNFLAGS = --name "nTexts" --240x320-support true --uses-lcd-blit true

ifeq ($(DEBUG),TRUE)
GCCFLAGS += -O0 -g
else
GCCFLAGS += -Os
endif

EXE = nTexts
OBJS = main.o app.o app_language.o app_log.o text_engine.o reader_index.o storage.o crc32.o chapter_rules.o
HOST_TEST = .build/tests/test_host
HOST_TEST_SOURCES = tests/test_host.c tests/host_fakes.c crc32.c chapter_rules.c text_engine.c reader_index.c storage.c

all: $(EXE).tns

%.o: %.c
	$(GCC) $(GCCFLAGS) -c $< -o $@

$(EXE).elf: $(OBJS)
	$(LD) $^ -o $@ $(LDFLAGS)

$(EXE).tns: $(EXE).elf
	$(GENZEHN) --input $< --output $@.zehn $(ZEHNFLAGS)
	make-prg $@.zehn $@
	rm -f $@.zehn

$(HOST_TEST): $(HOST_TEST_SOURCES)
	mkdir -p .build/tests
	$(HOST_CC) $(HOST_CFLAGS) -Itests/fakes -I. $(HOST_TEST_SOURCES) -o $@

clean:
	rm -f $(OBJS) $(EXE).elf $(EXE).tns $(EXE).tns.zehn
	rm -f $(HOST_TEST)
	rm -rf .build/host-test-data

test-static:
	python3 tests/test_static.py

test-host: $(HOST_TEST)
	rm -rf .build/host-test-data
	mkdir -p .build/host-test-data
	./$(HOST_TEST) .build/host-test-data

test: test-static test-host

.PHONY: all clean test test-static test-host
