CC = gcc
CFLAGS = -Wall -Werror -g -I. -Inm_modules -Iss_modules
LDFLAGS = -pthread

# Name server module object files
NM_OBJS = nm_modules/nm_hashmap.o nm_modules/nm_cache.o nm_modules/nm_logging.o nm_modules/nm_persistence.o

# Storage server module object files
SS_OBJS = ss_modules/ss_document.o ss_modules/ss_logging.o

all: nameserver storageserver client

# Compile name server modules
nm_modules/%.o: nm_modules/%.c nm_modules/%.h nm_modules/nm_types.h
	$(CC) $(CFLAGS) -c $< -o $@

# Compile storage server modules
ss_modules/%.o: ss_modules/%.c ss_modules/%.h ss_modules/ss_types.h
	$(CC) $(CFLAGS) -c $< -o $@

# Link nameserver with module objects
nameserver: nameserver.c $(NM_OBJS)
	$(CC) $(CFLAGS) nameserver.c $(NM_OBJS) -o nameserver $(LDFLAGS)

# Storage server - currently monolithic (can be modularized later)
storageserver: storageserver.c $(SS_OBJS)
	$(CC) $(CFLAGS) storageserver.c $(SS_OBJS) -o storageserver $(LDFLAGS)

# Client - simple, no modules needed
client: client.c
	$(CC) $(CFLAGS) client.c -o client $(LDFLAGS)

clean:
	rm -f nameserver storageserver client
	rm -f nm_modules/*.o ss_modules/*.o

.PHONY: all clean