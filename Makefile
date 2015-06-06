CC = gcc
CFLAGS = -Wall -g 
LDLIBS = -lpthread

OBJS = proxy.o csapp.o

all: proxy

proxy: $(OBJS)

csapp.o: csapp.c
	$(CC) $(CFLAGS) -c csapp.c

proxy.o: proxy.c
	$(CC) $(CFLAGS) -c proxy.c

clean:
	rm -f *~ *.o proxy core

