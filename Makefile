CFLAGS+=-I. -I credis 
LIBS=-ljson-c -lfcgi -lfcgi -lpthread
LDFLAGS=-l

%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<  $(LIBS)


credis: 
	make  -C credis

commetd: dm-main.o json-parser.o
	$(CC) $(CFLAGS) -o commetd $^ $(LIBS)

commet-cli: cli-main.o	
	$(CC) $(CFLAGS) -o commet-cli $^
		  

all:  credis  commetd commet-cli

clean:
	rm -f *.o
	make -C credis clean
       	
	 

dist-clean: clean

