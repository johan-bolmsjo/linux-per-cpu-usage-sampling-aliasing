CC := gcc
CFLAGS := -std=c23 -O2 -Wall -Wextra -Wpedantic

PROGRAMS := too-much-per-cpu-usage too-little-per-cpu-usage

.PHONY: all clean

all: $(PROGRAMS)

too-much-per-cpu-usage: too-much-per-cpu-usage.c
	$(CC) $(CFLAGS) -o $@ $<

too-little-per-cpu-usage: too-little-per-cpu-usage.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS)
