all: malt

CC=tcc
DEBUGCC=gcc

LIBS=
CFLAGS=-Os -pipe -s
DEBUGCFLAGS=-Og -pipe -g -pedantic -Wall -Wextra

INPUT=malt.c
OUTPUT=malt

RM=/bin/rm

.PHONY: malt
malt:
	$(CC) $(INPUT) -o $(OUTPUT) $(LIBS) $(CFLAGS)

debug:
	$(DEBUGCC) $(INPUT) -o $(OUTPUT) $(LIBS) $(DEBUGCFLAGS)

clean:
	if [ -e $(OUTPUT) ]; then $(RM) $(OUTPUT); fi
