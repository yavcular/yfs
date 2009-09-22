// the lock server implementation

#include "lock_server.h"
#include <sstream>
#include <map>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

lock_server::lock_server():
  nacquire (0)
{
  pthread_mutex_init(&_lock_mutex, NULL);
  pthread_cond_init(&_lock_cv, NULL);
}

lock_protocol::status
lock_server::stat(int clt, lock_protocol::lockid_t lid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;
  printf("stat request from clt %d\n", clt);
  r = nacquire;
  return ret;
}


lock_protocol::status
lock_server::acquire(lock_protocol::lockid_t lid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&_lock_mutex);
  while (_lock_map.count(lid) != 0 && _lock_map[lid]) {
    // wait till the other client releases the lock
    pthread_cond_wait(&_lock_cv, &_lock_mutex);
  }
  if (!_lock_map[lid]) {
    _lock_map[lid] = true;
  } else {
    ret = lock_protocol::RPCERR;
  }

  pthread_mutex_unlock(&_lock_mutex);

  return ret;
}

lock_protocol::status
lock_server::release(lock_protocol::lockid_t lid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&_lock_mutex);
  if (_lock_map.count(lid) != 0 && _lock_map[lid]) {
    _lock_map[lid] = false;
    pthread_cond_broadcast(&_lock_cv);
  } else {
    ret = lock_protocol::RPCERR;
  }
  pthread_mutex_unlock(&_lock_mutex);
  
  return ret;
}

