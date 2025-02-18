.phony: build clean

MPICC := mpicc
CFLAGS := -pthread -Wall
SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c, %.o, $(SRCS))

build: $(OBJS)
	$(MPICC) $(CFLAGS) -o tema2 $^

%.o: %.c
	$(MPICC) $(CFLAGS) -c $<

clean:
	rm -f *.o tema2
