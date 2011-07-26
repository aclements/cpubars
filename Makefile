cpubars: LDLIBS += -lncurses
cpubars: cpubars.o

clean:
	rm -f cpubars cpubars.o
