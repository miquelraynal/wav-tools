# SPDX-License-Identifier: GPL-2.0+

CC := $(CROSS_COMPILE)gcc
CFLAGS := -Wall -Wextra -Wpedantic -I. $(shell pkg-config --cflags gsl)
LIBS := $(shell pkg-config --libs gsl) # Requires libgsl-dev on Ubuntu

.PHONY: clean all

all: wav-generator wav-analyzer

wav-generator: wav-generator.o wav-lib.o *.h
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

wav-analyzer: wav-analyzer.o wav-lib.o *.h
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f wav-generator wav-analyzer *.o
