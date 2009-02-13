LAB=2
SOL=2
LAB4GE=$(shell expr $(LAB) \>\= 4)
LAB5GE=$(shell expr $(LAB) \>\= 5)
LAB6GE=$(shell expr $(LAB) \>\= 6)
CXXFLAGS =  -g -MD -Wall -DLAB=$(LAB) -DSOL=$(SOL) -D_FILE_OFFSET_BITS=64
FUSEFLAGS= -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=25 -I/usr/local/include/fuse -I/usr/include/fuse
ifeq ($(shell uname -s),Darwin)
MACFLAGS= -D__FreeBSD__=10
else
MACFLAGS=
endif
LDFLAGS = -L. -L/usr/local/lib
LDLIBS = -lrpc -lfuse -lpthread
CC = g++
CXX = g++

lab:  lab2
lab1: rpctest lock_server lock_tester lock_demo
lab2: yfs_client extent_server
lab3: yfs_client extent_server
lab4: yfs_client extent_server lock_server test-lab-4-b test-lab-4-c
lab5: yfs_client extent_server lock_server lock_tester test-lab-4-b\
	 test-lab-4-c
lab6: yfs_client extent_server lock_server test-lab-4-b test-lab-4-c

hfiles1=fifo.h chan.h host.h rpc.h marshall.h method_thread.h lock_protocol.h\
	 lock_server.h lock_client.h
hfiles2=yfs_client.h extent_client.h extent_protocol.h extent_server.h

rpclib=rpc.cc host.cc chan.cc
librpc.a: $(patsubst %.cc,%.o,$(rpclib))
	rm -f $@
	ar cq $@ $^
	ranlib librpc.a

rpctest=rpctest.cc
rpctest: $(patsubst %.cc,%.o,$(rpctest)) librpc.a

lock_demo=lock_demo.cc lock_client.cc
lock_demo : $(patsubst %.cc,%.o,$(lock_demo)) librpc.a

lock_tester=lock_tester.cc lock_client.cc
ifeq ($(LAB5GE),1)
lock_tester += lock_client_cache.cc
endif
lock_tester : $(patsubst %.cc,%.o,$(lock_tester)) librpc.a

lock_server=lock_server.cc lock_smain.cc
ifeq ($(LAB5GE),1)
lock_server+=lock_server_cache.cc
endif
lock_server : $(patsubst %.cc,%.o,$(lock_server)) librpc.a

yfs_client=yfs_client.cc extent_client.cc fuse.cc
ifeq ($(LAB4GE),1)
yfs_client += lock_client.cc
endif
ifeq ($(LAB5GE),1)
yfs_client += lock_client_cache.cc
endif
yfs_client : $(patsubst %.cc,%.o,$(yfs_client)) librpc.a

extent_server=extent_server.cc extent_smain.cc
extent_server : $(patsubst %.cc,%.o,$(extent_server)) librpc.a

test-lab-4-b=test-lab-4-b.c
test-lab-4-b:  $(patsubst %.c,%.o,$(test_lab_4-b)) librpc.a

test-lab-4-c=test-lab-4-c.c
test-lab-4-c:  $(patsubst %.c,%.o,$(test_lab_4-c)) librpc.a

fuse.o: fuse.cc
	$(CXX) -c $(CXXFLAGS) $(FUSEFLAGS) $(MACFLAGS) $<

l1:
	./mklab.pl 1 0 l1 GNUmakefile $(rpclib) $(rpctest) $(lock_server)\
	 $(lock_demo) $(lock_tester) $(hfiles1)

l2:
	./mklab.pl 2 0 l2 GNUmakefile $(yfs_client) $(extent_server) start.sh\
	 stop.sh test-lab-2.pl mkfs.sh $(hfiles2)

l3:
	./mklab.pl 3 0 l3 GNUmakefile  $(yfs_client) $(extent_server) start.sh\
	 stop.sh test-lab-2.pl mkfs.sh $(hfiles2) test3-lab-3.pl

l4:
	./mklab.pl 4 0 l4 GNUmakefile test3-lab-4-a.pl $(yfs_client)\
	 $(extent_server) start.sh stop.sh test-lab-2.pl mkfs.sh\
	 $(hfiles2) $(test-lab-4-b) $(test-lab-4-c)

l5:
	./mklab.pl 5 0 l5 GNUmakefile test3-lab-4-a.pl $(yfs_client)\
	 $(extent_server) start.sh stop.sh test-lab-2.pl mkfs.sh\
	 $(hfiles2) $(test-lab-4-b) $(test-lab-4-c)

l6:
	./mklab.pl 6 0 l6 GNUmakefile test3-lab-4-a.pl $(yfs_client)\
	 $(extent_server) start.sh stop.sh test-lab-2.pl mkfs.sh\
	 $(hfiles2) $(test-lab-4-b) $(test-lab-4-c)

-include *.d

.PHONY : clean
clean : 
	rm -rf *.o *.d librpc.a yfs_client extent_server lock_server lock_tester lock_demo rpctest 
