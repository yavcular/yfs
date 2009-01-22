// RPC stubs for clients to talk to lock_server

#include "lock_client.h"
#include "rpc.h"
#include <arpa/inet.h>

#include <sstream>
#include <iostream>
#include <stdio.h>

lock_client::lock_client(std::string dst)
{
	sockaddr_in dstsock;
	make_sockaddr(dst.c_str(), &dstsock);
  cl = new rpcc(dstsock);
  if (cl->bind() < 0) {
    printf("lock_client: call bind\n");
  }
}

int
lock_client::stat(std::string name)
{
  int r;
  int ret = cl->call(lock_protocol::stat, cl->id(), name, r);
  assert (ret == lock_protocol::OK);
  return r;
}

lock_protocol::status
lock_client::acquire(std::string name)
{
}

lock_protocol::status
lock_client::release(std::string name)
{
}

