CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11
LDFLAGS = -lm
PORT ?= 4242

COMMON_OBJ = net.o

all: dispatch passenger driver

dispatch: server.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o dispatch server.o $(COMMON_OBJ) $(LDFLAGS)

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

run-server: dispatch
	./dispatch $(PORT)

run-passenger: passenger
	./passenger 127.0.0.1 $(PORT) passenger1

run-driver: driver
	./driver 127.0.0.1 $(PORT) driver1

clean:
	rm -f *.o dispatch passenger driver