all:
	$(CC) occtest.c -o occtest

.PHONY: clean
clean:
	rm occtest
