CFLAGS+=-I. -I credis -DDM_DEBUG -Wall
LIBS=-ljson-c -lfcgi -lfcgi -lpthread -lcredis
LDFLAGS+=-L./credis 

LIBCREDIS=credis/libcredis.so

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<  $(LIBS)

all: commetd commet-cli

$(LIBCREDIS): 
	make  -C credis

commetd: $(LIBCREDIS) dm-main.o json-parser.o utils.o debug.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o commetd $^ $(LIBS)

commet-cli: cli-main.o	
	$(CC) $(CFLAGS) -o commet-cli $^
		  

clean:
	rm commetd commet-cli
	rm -f *.o
	make -C credis clean
       	
	 

dist-clean: clean

