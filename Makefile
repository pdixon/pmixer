CC = clang
CCFLAGS = -lpulse -Wall

all: pmixer

pmixer: pmixer.c
	$(CC) $(CCFLAGS) -o $@ pmixer.c

clean:
	rm -f pmixer
