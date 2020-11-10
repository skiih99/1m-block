all: 1m-block

1m-block: 1mblock.o
	gcc -o 1m-block 1mblock.o -lnetfilter_queue

1mblock.o: 
	gcc -c -o 1mblock.o 1mblock.c

clean:
	rm -f 1m-block
	rm -f *.o