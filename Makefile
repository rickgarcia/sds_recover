C=gcc
LDFLAGS=-L. -lc
CFLAGS=-c -g -Wall
IFLAGS=-I.
SOURCES=sds_recover.c 
OBJECTS = $(SOURCES:.c=.o)
TARGET=sds_recover

.c.o:
	$(CC) $(CFLAGS) $(IFLAGS) $< -o $@

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

clean:
	rm -f *.o $(TARGET)
