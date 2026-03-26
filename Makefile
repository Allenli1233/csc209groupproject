CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11

COMMON_OBJ = net.o

all: dispatch passenger driver

dispatch: server.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o dispatch server.o $(COMMON_OBJ)

passenger: passenger.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o passenger passenger.o $(COMMON_OBJ)

driver: driver.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o driver driver.o $(COMMON_OBJ)

server.o: server.c protocol.h net.h
	$(CC) $(CFLAGS) -c server.c

passenger.o: passenger.c protocol.h net.h
	$(CC) $(CFLAGS) -c passenger.c

driver.o: driver.c protocol.h net.h
	$(CC) $(CFLAGS) -c driver.c

net.o: net.c net.h protocol.h
	$(CC) $(CFLAGS) -c net.c

clean:
	rm -f *.o dispatch passenger driver