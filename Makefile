
BINS := mcdist

all: $(BINS)

CFLAGS += -Wall -pthread
CC = gcc
LDFLAGS += -pthread

../%: %
	cp $< $@

mcdist : CFLAGS += -O3

mcdist: mcdist.o

clean:
	rm -f $(BINS) *.o

.PHONY: all clean
