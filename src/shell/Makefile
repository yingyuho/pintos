CC = gcc
CFLAGS = -Wall -W -g

all: mysh

mysh: mysh.o

parsertest: parser.o stringbuilder.o
	$(CC) -o parsertest parser.o stringbuilder.o

clean: 
	rm -f *.o *~ mysh parsertest

.PHONY: all clean


