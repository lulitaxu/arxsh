CC = cc
CFLAGS = -std=c11 -Wall -Wextra -g

arxsh: src/main.c
	$(CC) $(CFLAGS) -o arxsh src/main.c

clean:
	rm -f arxsh

.PHONY: clean
