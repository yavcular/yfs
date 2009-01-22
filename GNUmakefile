LAB=1
SOL=1
CXXFLAGS =  -g -MD -Wall -DLAB=$(LAB) -DSOL=$(SOL) -D_FILE_OFFSET_BITS=64
LDFLAGS = -L.
LDLIBS = -lrpc -lpthread
CC = g++
CXX = g++

lab1: librpc.a rpctest lock_server lock_tester lock_demo

hfiles=fifo.h chan.h host.h rpc.h marshall.h method_thread.h lock_protocol.h lock_server.h\
	 lock_client.h

rpclib=rpc.cc host.cc chan.cc
librpc.a: $(patsubst %.cc,%.o,$(rpclib))
	rm -f $@
	ar cq $@ $^
	ranlib librpc.a

rpctest=rpctest.cc
rpctest: $(patsubst %.cc,%.o,$(rpctest))

lock_server = lock_server.cc lock_smain.cc
lock_server : $(patsubst %.cc,%.o,$(lock_server))

lock_demo = lock_demo.cc lock_client.cc
lock_demo : $(patsubst %.cc,%.o,$(lock_demo))

lock_tester = lock_tester.cc lock_client.cc 
lock_tester : $(patsubst %.cc,%.o,$(lock_tester))

l1:
	./mklab.pl 1 0 l1 GNUmakefile $(rpclib) $(rpctest) $(lock_server) $(lock_demo) $(lock_tester) $(hfiles)

-include *.d

.PHONY : clean
clean : 
	rm -rf *.o *.d librpc.a fuse2yfs extent_server yfs_client lock_server lock_tester lock_demo rpctest 
