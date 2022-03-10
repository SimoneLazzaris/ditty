CC=gcc
CFLAGS=-Wall
all: ditty ditty_android
android: ditty_android

ditty: ditty.c
	$(CC) $(CFLAGS) $^ -o $@

ditty_android: ditty_android.c
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: all clean

clean:
	-rm -f ditty ditty_android
