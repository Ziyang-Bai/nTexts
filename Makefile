DEBUG ?= FALSE

GCC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn

GCCFLAGS = -Wall -Wextra -marm -std=c99
LDFLAGS = -Wl,--nspireio,--gc-sections
ZEHNFLAGS = --name "nTexts" --240x320-support true --uses-lcd-blit true

ifeq ($(DEBUG),TRUE)
GCCFLAGS += -O0 -g
else
GCCFLAGS += -Os
endif

EXE = nTexts
OBJS = main.o app.o app_language.o app_log.o text_engine.o reader_index.o storage.o

all: $(EXE).tns

%.o: %.c
	$(GCC) $(GCCFLAGS) -c $< -o $@

$(EXE).elf: $(OBJS)
	$(LD) $^ -o $@ $(LDFLAGS)

$(EXE).tns: $(EXE).elf
	$(GENZEHN) --input $< --output $@.zehn $(ZEHNFLAGS)
	make-prg $@.zehn $@
	rm -f $@.zehn

clean:
	rm -f $(OBJS) $(EXE).elf $(EXE).tns $(EXE).tns.zehn

test:
	python3 tests/test_static.py

.PHONY: all clean test
