CFLAGS+=-std=gnu99 -I. -DDM_DEBUG -DCLI_DEBUG -DB_DEBUG -Wall -Wextra -Wno-unused-parameter -g
LIBS=-ljson-c -lfcgi -lpthread -lhiredis

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<  $(LIBS)

all: commetd commet-cli

commetd: dm-main.o utils.o debug.o libdio.o db.o json-parser.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o commetd $^ $(LIBS)

commet-cli: cli-main.o utils.o debug.o libdio.o 	
	$(CC) $(CFLAGS) -o commet-cli $^
		  

clean:
	rm -f commetd commet-cli
	rm -f *.o       	
	 

dist-clean: clean

