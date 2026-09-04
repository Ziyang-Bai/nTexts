DEBUG ?= FALSE

GCC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn
HOST_CC ?= cc
HOST_CFLAGS ?= -Wall -Wextra -std=c99

GCCFLAGS = -Wall -Wextra -marm -std=c99 -MMD -MP
LDFLAGS = -Wl,--nspireio,--gc-sections
ZEHNFLAGS = --name "nTexts" --240x320-support true --uses-lcd-blit true

ifeq ($(DEBUG),TRUE)
GCCFLAGS += -O0 -g
else
GCCFLAGS += -Os
endif

EXE = nTexts
OBJS = main.o app.o app_language.o app_log.o text_engine.o reader_index.o storage.o crc32.o chapter_rules.o file_replace.o
DEPS = $(OBJS:.o=.d)
HOST_TEST = .build/tests/test_host
HOST_TEST_SOURCES = tests/test_host.c tests/host_fakes.c crc32.c chapter_rules.c file_replace.c text_engine.c reader_index.c storage.c
HOST_TEST_HEADERS = chapter_rules.h crc32.h file_replace.h reader_index.h storage.h text_engine.h \
	tests/fakes/libndls.h tests/fakes/ngc.h

all: $(EXE).tns

%.o: %.c
	$(GCC) $(GCCFLAGS) -c $< -o $@

$(EXE).elf: $(OBJS)
	$(LD) $^ -o $@ $(LDFLAGS)

$(EXE).tns: $(EXE).elf
	$(GENZEHN) --input $< --output $@.zehn $(ZEHNFLAGS)
	make-prg $@.zehn $@
	rm -f $@.zehn

$(HOST_TEST): $(HOST_TEST_SOURCES) $(HOST_TEST_HEADERS)
	mkdir -p .build/tests
	$(HOST_CC) $(HOST_CFLAGS) -Itests/fakes -I. $(HOST_TEST_SOURCES) -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(EXE).elf $(EXE).tns $(EXE).tns.zehn
	rm -f $(HOST_TEST)
	rm -rf .build/host-test-data

test-static:
	python3 tests/test_static.py

test-host: $(HOST_TEST)
	rm -rf .build/host-test-data
	mkdir -p .build/host-test-data
	./$(HOST_TEST) .build/host-test-data

test: test-static test-host

-include $(DEPS)

.PHONY: all clean test test-static test-host
