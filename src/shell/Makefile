CC = gcc
CFLAGS = -Wall -W -g

all: mysh

mysh: mysh.o parser.o stringbuilder.o
	$(CC) $(CFLAGS) -o mysh $^

clean: 
	rm -f *.o *~ mysh

.PHONY: all clean


