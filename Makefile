CFLAGS=-g -Wall -std=c99 -D_POSIX_SOURCE=1 -D_XOPEN_SOURCE=500
linux-si470x: linux-si470x.c
	$(CC) $(CFLAGS) -o $@ $< -lasound -ljack -lm -lsamplerate
