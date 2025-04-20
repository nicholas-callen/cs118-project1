CC=gcc
CPPFLAGS=-Wall -Wextra 
LDFLAGS= 
LDLIBS=

DEPS=transport.o io.o

all: server client 

server: server.o $(DEPS)
client: client.o $(DEPS)

clean:
	@rm -rf server client *.bin *.o	
