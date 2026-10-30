CC = cc
CFLAGS = -std=c11 -Wall -Wextra -g

arxsh: src/main.c
	$(CC) $(CFLAGS) -o arxsh src/main.c

test: arxsh
	sh tests/run_integration.sh
	python3 tests/run_pty.py

test-asan:
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -o arxsh_asan src/main.c
	ARXSH_BIN=./arxsh_asan sh tests/run_integration.sh
	rm -f arxsh_asan

clean:
	rm -f arxsh arxsh_asan

.PHONY: clean test test-asan
