// RPC layer.  Thread per request. Client thread performs retransmissions
// chan.cc sends and receive PDUs from network

// GC combined w. exceptions might make threads+locks+delete much easier.

// TODO:
// vivaldi

#include "rpc.h"
#include "method_thread.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <strings.h>
#include <sys/select.h>
#include <time.h>
#include <list>
#include <vector>

rpcc::rpcc(sockaddr_in _dst, bool _debug)
  : dst(_dst), debug(_debug), xid(1), svr_nonce(0), bind_done(false), chan(_dst, _debug), _vivaldi(NULL), destroy_wait (false)
{
  assert(pthread_mutex_init(&m, 0) == 0);
  assert(pthread_mutex_init(&_timeout_lock, 0) == 0);
  assert(pthread_cond_init(&_timeout_cond, 0) == 0);
  assert(pthread_cond_init(&destroy_wait_c, 0) == 0);
  _next_timeout.tv_sec = 0;

  // Create a thread that runs clock_loop to enable retransmissions
  // ** YOU FILL THIS IN FOR LAB 1 **
  if ((th_clock_loop = method_thread(this, false, &rpcc::clock_loop)) == 0) {
    perror("rpcc::rpcc pthread_create");
    exit(1);
  }

  if((th_chan_loop = method_thread(this, false, &rpcc::chan_loop)) == 0){
    perror("rpcc::rpcc pthread_create");
    exit(1);
  }

  // set clt_nonce to a random number to make this instance unique
  struct timeval tv;
  gettimeofday(&tv, NULL);
  srandom(tv.tv_usec);
  clt_nonce = random();

  xid_rep_window.push_back(0);

  if (debug) printf("rpcc: my nonce %d\n", clt_nonce);
}


rpcc::~rpcc()
{
  if (debug) printf("~rpcc\n");

  assert(pthread_cancel(th_clock_loop) == 0);
  assert(pthread_cancel(th_chan_loop) == 0);

  assert(pthread_join(th_clock_loop, NULL) == 0);
  assert(pthread_join(th_chan_loop, NULL) == 0);

  calls.clear();
  xid_rep_window.clear();

  assert(pthread_mutex_destroy(&m) == 0);
  assert(pthread_mutex_destroy(&_timeout_lock) == 0);
  assert(pthread_cond_destroy(&_timeout_cond) == 0);
  assert(pthread_cond_destroy(&destroy_wait_c) == 0);
  if (debug) printf("~rpcc done\n");
}

void 
rpcc::setlossy(bool x)
{
  if (x)
    chan.setlossy();
  else
    chan.setlossy(0);
}

void 
rpcc::cancel(void)
{
  assert(pthread_mutex_lock(&m) == 0);
  std::map<int,caller*>::iterator iter;
  for(iter = calls.begin(); iter != calls.end(); iter++){
    caller *ca = iter->second;
    if (debug) printf ("cancel: force caller to fail\n");
    assert(pthread_mutex_lock(&ca->m) == 0);
    ca->done = true;
    ca->intret = rpc_const::cancel_failure;
    assert(pthread_cond_signal(&ca->c) == 0);
    assert(pthread_mutex_unlock(&ca->m) == 0);
  }

  while (calls.size () > 0) {
    destroy_wait = true;
    assert(pthread_cond_wait(&destroy_wait_c,&m) == 0);
  }

  assert(pthread_mutex_unlock(&m) == 0);
}

// bind this rpc to a rpcs instance; should be done exactly once.
// necessary to ensure at-most-once RPC semantics.
int 
rpcc::bind(TO to) 
{
  int r;
  int ret = call(rpc_const::bind, 0, r, to);
  if (ret == 0) {
    assert(pthread_mutex_lock(&m) == 0);
    svr_nonce = r;
    bind_done = true;
    assert(pthread_mutex_unlock(&m) == 0);
  }
  return ret;
};

rpcc::caller::caller(int xxid, unmarshall *xun, uint32_t ip, uint16_t port)
  : xid(xxid), un(xun), done(false), other_ip(ip), other_port(port)
{
  assert(pthread_cond_init(&c, 0) == 0);
  assert(pthread_mutex_init(&m, 0) == 0);
}

rpcc::caller::~caller()
{
  assert(pthread_mutex_destroy(&m) == 0);
  assert(pthread_cond_destroy(&c) == 0);
}

const rpcc::TO rpcc::to_inf = { -1 };

/* Subtract the `struct timeval' values X and Y,
   storing the result in RESULT.
   Return 1 if the difference is negative, otherwise 0.  */
// taken from http://www.delorie.com/gnu/docs/glibc/libc_428.html
int
timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y)
{
  struct timeval y2 = *y;

  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y2.tv_usec) {
    int nsec = (y2.tv_usec - x->tv_usec) / 1000000 + 1;
    y2.tv_usec -= 1000000 * nsec;
    y2.tv_sec += nsec;
  }
  if (x->tv_usec - y2.tv_usec > 1000000) {
    int nsec = (x->tv_usec - y2.tv_usec) / 1000000;
    y2.tv_usec += 1000000 * nsec;
    y2.tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y2.tv_sec;
  result->tv_usec = x->tv_usec - y2.tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y2.tv_sec;
}

void
add_timeout(struct timeval a, int b, struct timespec *result, 
      struct timeval *result_tv) {
  // convert to millisec, add timeout, convert back
  long double msec = (a.tv_sec*1000.0 + a.tv_usec/1000.0)+b*1.0;
  result->tv_sec = (long) (msec/1000);
  result->tv_nsec = 
    ((long) ((msec-((long double) result->tv_sec*1000.0))*1000))*1000;
  result_tv->tv_sec = result->tv_sec;
  result_tv->tv_usec = result->tv_nsec/1000;
}

int
rpcc::call1(unsigned int proc, const marshall &req, unmarshall &rep, TO to)
{
  assert (pthread_mutex_lock(&m) == 0);

  if ((proc != rpc_const::bind && !bind_done) || 
      (proc == rpc_const::bind && bind_done)) {
    fprintf(stderr, "rpcc::call1 rpcc has not been bound to server or binding twice\n");
    assert (pthread_mutex_unlock(&m) == 0);
    return rpc_const::bind_failure;
  }

  if (destroy_wait) {
    assert (pthread_mutex_unlock(&m) == 0);
    return rpc_const::cancel_failure;
  }

  unsigned int myxid = xid++;
  marshall m1;

  caller ca(myxid, &rep, dst.sin_addr.s_addr, dst.sin_port);
  calls[myxid] = &ca;

  // add RPC fields before req
  m1 << clt_nonce << svr_nonce << proc << myxid << xid_rep_window.front() << req.str();

  assert(pthread_mutex_unlock(&m) == 0);

  struct timeval first;
  gettimeofday(&first, NULL);
  struct timeval last = first;
  int initial_rto = 250; // initial timeout (msec)
  int rto = initial_rto;
  int next_rto = rto;

  struct timespec deadline;
  struct timeval deadline_tv;
  if(to.to != -1) {
    add_timeout(first, to.to, &deadline, &deadline_tv);
  }

  assert (pthread_mutex_lock(&ca.m) == 0);

  struct timeval now, diff;
  gettimeofday(&now, NULL);
  while(1){
    if(ca.done)
      break;
    assert(timeval_subtract(&diff, &now, &last) == 0);
    long double diff_msec = diff.tv_sec*1000.0 + diff.tv_usec/1000.0;
    if(diff_msec >= rto || next_rto == initial_rto){
      rto = next_rto;
      if(rto != initial_rto)
        if (debug) printf("rpcc::call1 retransmit proc %x xid %d %s:%d\n", 
                proc, myxid,
                inet_ntoa(dst.sin_addr),
                ntohs(dst.sin_port));
      chan.send(m1.str());
      gettimeofday(&last, NULL);
      // increase rxmit timer for next time
      next_rto *= 2;
      if(next_rto > 128000)
        next_rto = 128000;
    }

    // rexmit deadline
    struct timespec my_deadline;
    struct timeval my_deadline_tv;
    add_timeout(last, rto, &my_deadline, &my_deadline_tv);

    // my next deadline is either for rxmit or timeout
    if(to.to != -1 && 
       timeval_subtract(&diff, &deadline_tv, &my_deadline_tv) > 0) {
      my_deadline = deadline;
      my_deadline_tv = deadline_tv;
    }

    assert(pthread_mutex_lock(&_timeout_lock) == 0);
    struct timeval nt_tv;
    nt_tv.tv_sec = _next_timeout.tv_sec;
    nt_tv.tv_usec = _next_timeout.tv_nsec/1000;
    if(_next_timeout.tv_sec == 0 || 
       timeval_subtract(&diff, &my_deadline_tv, &nt_tv) > 0){
      _next_timeout = my_deadline;
      pthread_cond_signal(&_timeout_cond);
    }
    assert(pthread_mutex_unlock(&_timeout_lock) == 0);

    // wait for reply or timeout
    assert(pthread_cond_wait(&ca.c, &ca.m) == 0);

    // user-specified timeout occurred
    gettimeofday(&now, NULL);
    if(!ca.done && to.to != -1){
      if(timeval_subtract(&diff, &deadline_tv, &now) > 0)
        break;
    }

  }

  int intret = ca.done ? ca.intret : rpc_const::timeout_failure;
  pthread_mutex_unlock(&ca.m);

  assert(pthread_mutex_lock(&m) == 0);
  calls.erase(myxid);
  if (destroy_wait) {
    assert(pthread_cond_signal(&destroy_wait_c) == 0);
  }
  assert(pthread_mutex_unlock(&m) == 0);

  return intret;
}

// listen for replies, hand to xid's waiting rpcc::call().
void
rpcc::chan_loop()
{
  th_chan_loop = pthread_self();
  int oldstate;
  assert(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate) == 0);

  while(1){
    std::string s = chan.recv();
    unmarshall rep(s);
    got_reply(rep);
  }
}

// periodically wakes up all callers so they can think
// about retransmitting.
void
rpcc::clock_loop()
{
  th_clock_loop = pthread_self();
  int oldstate;
  assert(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate) == 0);

  while(1){

    // if there are no timeouts, wake until there is one
    assert(pthread_mutex_lock(&_timeout_lock) == 0);
    bool signalled = false;
    pthread_cleanup_push(&rpcc::cleanup_timeout_lock, 
         (void *) &_timeout_lock);
    if(_next_timeout.tv_sec == 0){
      pthread_cond_wait(&_timeout_cond, &_timeout_lock);
      signalled = true;
    } else {
      signalled = 
  (pthread_cond_timedwait(&_timeout_cond, &_timeout_lock, &_next_timeout)
   != ETIMEDOUT);
    }
    pthread_cleanup_pop(0);

    // if we were signalled to be woken up, that means our timeout value
    // was set to something new, so we just need to wait again, and not
    // bother waking everyone up
    if(!signalled)
      _next_timeout.tv_sec = 0;
    assert(pthread_mutex_unlock(&_timeout_lock) == 0);
    if(signalled)
      continue;

    assert(pthread_mutex_lock(&m) == 0);
    std::map<int,caller*>::iterator iter;
    for(iter = calls.begin(); iter != calls.end(); iter++){
      caller *ca = iter->second;
      assert(pthread_mutex_lock(&ca->m) == 0);
      assert(pthread_cond_broadcast(&ca->c) == 0);
      assert(pthread_mutex_unlock(&ca->m) == 0);
    }
    assert(pthread_mutex_unlock(&m) == 0);
  }
}

void
rpcc::got_reply(unmarshall &rep)
{
  int xid, intret;
  rep >> xid;
  if(!rep.ok())
    return;

  assert(pthread_mutex_lock(&m) == 0);

  if(calls.count(xid) < 1){
    assert(pthread_mutex_unlock(&m) == 0);
    return;
  }
        
  caller *ca = calls[xid];
  update_xid_rep(xid);

  assert(pthread_mutex_lock(&ca->m) == 0);

  rep >> intret;
  ca->un->str(rep.istr());
  ca->intret = intret;
  ca->done = true;
  assert(pthread_cond_broadcast(&ca->c) == 0);
  assert(pthread_mutex_unlock(&ca->m) == 0);
  
  // Wait until down here to unlock the global lock.  This avoids the
  // case where got_reply() gets the caller from the map and locks ca->m, 
  // then call1() erases the caller from the map and tries to destroy the
  // caller object before ca->m is unlocked, causing disaster.
  assert(pthread_mutex_unlock(&m) == 0);

}

// assumes thread holds mutex m
void rpcc::update_xid_rep(unsigned int xid)
{
  std::list<unsigned int>::iterator it;

  if (xid <= xid_rep_window.front()) {
    return;
  }

  for (it = xid_rep_window.begin(); it != xid_rep_window.end(); it++) {
    if (*it > xid) {
      xid_rep_window.insert(it, xid);
      goto compress;
    }
  }
  xid_rep_window.push_back(xid);

 compress:
  // removes any continous natural sequence, leaving the last item in the
  // sequence, e.g., 0, 1, 2, 5, 8 ---(removing 0 and 1) --> 2, 5, 8
  it = xid_rep_window.begin();
  for (it++; it != xid_rep_window.end(); it++) {
    if (xid_rep_window.front() + 1 == *it)
      xid_rep_window.pop_front();
  }
}

rpcs::rpcs(unsigned int port, bool _debug)
  : debug (_debug), chan(port, _debug), _vivaldi(NULL), nthread(0), deleting(false), counting(0) 
{
  assert(pthread_mutex_init(&procs_m, 0) == 0);
  assert(pthread_mutex_init(&reply_window_m, 0) == 0);

  assert(pthread_mutex_init(&delete_m, 0) == 0);
  assert(pthread_cond_init(&delete_c, 0) == 0);

  // set server nonce to a random value to make this instance unique
  struct timeval tv;
  gettimeofday(&tv, NULL);
  srandom(tv.tv_usec);
  nonce = random();

  char *count_env = getenv("RPC_COUNT");
  if(count_env != NULL){
    counting = atoi(count_env);
  }

  if((th_loop = method_thread(this, false, &rpcs::loop)) == 0){
    perror("rpcs::rpcs pthread_create");
    exit(1);
  }

  reg(rpc_const::bind, this, &rpcs::bind);
}

rpcs::~rpcs()
{
  if (debug) printf("~rpcs\n");
  assert (pthread_cancel(th_loop) == 0);
  assert (pthread_mutex_lock(&delete_m) == 0);
  deleting = true;
  while (nthread > 0) {
    if (debug) printf("~rpcs: wait for handlers\n");
    assert (pthread_cond_wait(&delete_c, &delete_m) == 0);
  }
  assert (pthread_mutex_unlock(&delete_m) == 0);
  pthread_join(th_loop, NULL);

  free_reply_window();

  assert(pthread_mutex_destroy(&delete_m) == 0);
  assert(pthread_cond_destroy(&delete_c) == 0);
  assert(pthread_mutex_destroy(&reply_window_m) == 0);
  assert(pthread_mutex_destroy(&procs_m) == 0);

  if (debug) printf("~rpcs done\n");
}

void rpcs::inc_nthread()
{
  assert (pthread_mutex_lock(&delete_m) == 0);
  nthread++;
  assert (pthread_mutex_unlock(&delete_m) == 0);
  assert (pthread_cond_signal(&delete_c) == 0);
}

void rpcs::dec_nthread()
{
  assert (pthread_mutex_lock(&delete_m) == 0);
  nthread--;
  assert (pthread_mutex_unlock(&delete_m) == 0);
  assert (pthread_cond_signal(&delete_c) == 0);
}

void
rpcs::setlossy(bool x)
{
  if (x)
    chan.setlossy();
  else
    chan.setlossy(0);
}

handler::handler()
{
}

void
rpcs::reg1(unsigned int proc, handler *h)
{
  assert(pthread_mutex_lock(&procs_m) == 0);
  assert(procs.count(proc) == 0);
  procs[proc] = h;
  assert(procs.count(proc) >= 1);
  assert(pthread_mutex_unlock(&procs_m) == 0);
}

void 
rpcs::updatestat(unsigned int proc)
{
  counts[proc]++;
  counting--;
  if( counting == 0 ) {
    std::map<int, int>::iterator i;
    printf( "RPC STATS: " );
    for( i = counts.begin(); i != counts.end(); i++ ) {
      printf( "%x:%d ", i->first, i->second );
    }
    printf("\n");
    char *count_env = getenv("RPC_COUNT");
    if(count_env != NULL){
      counting = atoi(count_env);
    }
  }
}


void
rpcs::dispatch(junk *j)
{
  std::string xreq = j->s;
  int channo = j->chan;
  delete j;

  unmarshall req(xreq);
  int clt_nonce = req.i32();
  int srv_nonce = req.i32();
  unsigned int proc = req.i32();
  unsigned int xid = req.i32();
  unsigned int rep_xid = req.i32();
  unmarshall args(req.istr());
  marshall rep;
  marshall rep1;
  int ret;
  handler *h;

  assert(pthread_mutex_lock(&procs_m) == 0);

  if(!req.ok() || procs.count(proc) < 1){
    assert(pthread_mutex_unlock(&procs_m) == 0);
    if (debug) printf("bad proc %x req ok? %d procs.count %u\n",
            proc, req.ok()?1:0, procs.count(proc));
    goto exit;
  }

  h = procs[proc];

  assert(pthread_mutex_unlock(&procs_m) == 0);

  if (debug)
    printf ("rpc %u (last_rep %u) from clt %u for srv instance %u \n", xid, 
      rep_xid, clt_nonce, srv_nonce);

  if (nonce != 0 && srv_nonce != 0 && srv_nonce != nonce) {
    if (debug) 
      printf("rpc for an old server instance %u (current %u) proc %x\n", 
      srv_nonce, nonce, proc);
    rep1 << xid;
    rep1 << rpc_const::atmostonce_failure;
    chan.send(rep1.str(), channo);
    goto exit;
  }

  switch (checkduplicate_and_update(clt_nonce, xid, rep_xid, rep1)) {
  case NEW: // this is a new request
    if (counting)
      updatestat(proc);
    ret = h->fn(args, rep);
    rep1 << xid;
    rep1 << ret;
    rep1 << rep.str();
    add_reply(clt_nonce, xid, rep1);
    chan.send(rep1.str(), channo);
    break;
  case INPROGRESS: // server is working on this request
    break;
  case DONE: // duplicate and we still have the response 
    chan.send(rep1.str(), channo);
    break;
  case FORGOTTEN: // very old request; don't have response anymore
    if (debug) 
      printf("very old request %u from %u\n", xid, clt_nonce);
    rep1 << xid;
    rep1 << rpc_const::atmostonce_failure;
    chan.send(rep1.str(), channo);
    break;
  }

 exit:
  dec_nthread();
}


void rpcs::add_reply(unsigned int clt_nonce, unsigned int xid,
             marshall &rep)
{
  assert(pthread_mutex_lock(&reply_window_m) == 0);

  // remember the reply for this xid.
  // ** YOU FILL THIS IN FOR LAB 1 **

  std::list<reply_t *> &replies = reply_window[clt_nonce];
  std::list<reply_t *>::iterator itr = replies.begin();
  while (itr != replies.end()) {
    if ( (*itr)->xid == xid) {
      (*itr)->rep_present = true;
      (*itr)->rep = rep;
      break;
    }
    ++itr;
  }

  assert(pthread_mutex_unlock(&reply_window_m) == 0);
}

void 
rpcs::free_reply_window(void)
{
  std::map<unsigned int,std::list<reply_t *> >::iterator clt;
  std::list<reply_t *>::iterator it;

  assert(pthread_mutex_lock(&reply_window_m) == 0);
  for (clt = reply_window.begin(); clt != reply_window.end(); clt++) {
    for (it = clt->second.begin(); it != clt->second.end(); it++) {
      delete *it;
    }
    clt->second.clear();
  }
  reply_window.clear();
  assert(pthread_mutex_unlock(&reply_window_m) == 0);
}

rpcs::rpcstate_t rpcs::checkduplicate_and_update(unsigned int clt_nonce,
	     unsigned int xid, unsigned int xid_rep, marshall &rep)
{
  // xid: the xid of the incoming rpc request
  // xid_rep: ack the client has received all replies whose xid <= xid_rep (meaning
  // we can safely move the sliding window forward to xid_rep

  rpcstate_t r = NEW;

  assert(pthread_mutex_lock(&reply_window_m) == 0);

  // check if xid is a duplicate, and if not update list of received xid, so
  // that checking and update is atomic.
  // fill in xid_rep if we have the reply (state is DONE)

  // ** YOU FILL THIS IN FOR LAB 1 **

  std::list<reply_t *> &replies = reply_window[clt_nonce];
  std::list<reply_t *>::iterator it;

  if (!replies.empty() && xid <= replies.front()->xid) {
    r = FORGOTTEN;
    goto out;
  }

  for (it = replies.begin(); it != replies.end(); it++) {
    if ((*it)->xid == xid) {
      // this xid is a duplicate
      if ( (*it)->rep_present) {
        r = DONE;
      } else {
        r = INPROGRESS;
      }
      goto out;
    } else if ( (*it)->xid > xid) {
      replies.insert(it, new reply_t(xid));
      goto compress;
    }
  }
  replies.push_back(new reply_t(xid));

compress:
  // Safely removes all those items whose id < xid_rep to make it
  // comply with the xid_rep_window on client
  it = replies.begin();
  while (replies.front()->xid < xid_rep) {
    replies.pop_front();
  }

out:
  assert(pthread_mutex_unlock(&reply_window_m) == 0);
  return r;
}


rpcs::junk::junk(std::string xs, int xchan)
  : s(xs), chan (xchan)
{ }

void
rpcs::loop()
{
  int oldstate;
  assert(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate) == 0);

  while(1){
    std::string req;
    int channo;

    chan.recv(req, channo);

    if(req.size() < 8)
      continue;

    junk *j = new junk(req, channo);

    assert (pthread_mutex_lock(&delete_m) == 0);
    if (!deleting) {
      nthread++;
      if(method_thread(this, true, &rpcs::dispatch, j) == 0){
        perror("rpcs::loop pthread_create");
        nthread--;
      }
    }
    assert (pthread_mutex_unlock(&delete_m) == 0);
  }
}

int rpcs::bind(int a, int &r)
{ 
  if (debug) printf("rpcs::bind called nonce %u\n", nonce);
  r = nonce;
  return 0;
}

void
marshall::rawbyte(unsigned x)
{
  s.put((unsigned char) x);
}

void
marshall::rawbytes(const char *p, int n)
{
  s.write(p, n);
}

marshall &
operator<<(marshall &m, unsigned char x)
{
  m.rawbyte(x);
  return m;
}

marshall &
operator<<(marshall &m, char x)
{
  m << (unsigned char) x;
  return m;
}


marshall &
operator<<(marshall &m, unsigned short x)
{
  m.rawbyte(x & 0xff);
  m.rawbyte((x >> 8) & 0xff);
  return m;
}

marshall &
operator<<(marshall &m, short x)
{
  m << (unsigned short) x;
  return m;
}

marshall &
operator<<(marshall &m, unsigned int x)
{
  m.rawbyte(x & 0xff);
  m.rawbyte((x >> 8) & 0xff);
  m.rawbyte((x >> 16) & 0xff);
  m.rawbyte((x >> 24) & 0xff);
  return m;
}

marshall &
operator<<(marshall &m, unsigned long x)
{
  m.rawbyte(x & 0xff);
  m.rawbyte((x >> 8) & 0xff);
  m.rawbyte((x >> 16) & 0xff);
  m.rawbyte((x >> 24) & 0xff);
  return m;
}

marshall &
operator<<(marshall &m, int x)
{
  m << (unsigned int) x;
  return m;
}

marshall &
operator<<(marshall &m, const std::string &s)
{
  m << (unsigned int) s.size();
  m.rawbytes(s.data(), s.size());
  return m;
}

marshall &
operator<<(marshall &m, unsigned long long x)
{
  m << (unsigned int) x;
  m << (unsigned int) (x >> 32);
  return m;
}


bool
unmarshall::okdone()
{
  if(ok() && ((int)s.tellg() == (int)s.str().size())){
    return true;
  } else {
    // an RPC request or reply was too short or too long.
#if 0
    unsigned int *p = (unsigned int *) s.str().data();
    fprintf(stderr, "okdone failed len=%d %x %x %x\n",
            s.str().size(), p[0], p[1], p[2]);
#endif
    return false;
  }
}

unsigned int
unmarshall::rawbyte()
{
  char c = 0;
  if(!s.get(c))
    _ok = false;
  return c;
}

unmarshall &
operator>>(unmarshall &u, unsigned char &x)
{
  x = (unsigned char) u.rawbyte() ;
  return u;
}

unmarshall &
operator>>(unmarshall &u, char &x)
{
  x = (char) u.rawbyte();
  return u;
}


unmarshall &
operator>>(unmarshall &u, unsigned short &x)
{
  x = u.rawbyte() & 0xff;
  x |= (u.rawbyte() & 0xff) << 8;
  return u;
}

unmarshall &
operator>>(unmarshall &u, short &x)
{
  x = u.rawbyte() & 0xff;
  x |= (u.rawbyte() & 0xff) << 8;
  return u;
}

unmarshall &
operator>>(unmarshall &u, unsigned int &x)
{
  x = u.rawbyte() & 0xff;
  x |= (u.rawbyte() & 0xff) << 8;
  x |= (u.rawbyte() & 0xff) << 16;
  x |= (u.rawbyte() & 0xff) << 24;
  return u;
}

unmarshall &
operator>>(unmarshall &u, unsigned long &x)
{
  x = u.rawbyte() & 0xff;
  x |= (u.rawbyte() & 0xff) << 8;
  x |= (u.rawbyte() & 0xff) << 16;
  x |= (u.rawbyte() & 0xff) << 24;
  return u;
}

unmarshall &
operator>>(unmarshall &u, int &x)
{
  x = u.rawbyte() & 0xff;
  x |= (u.rawbyte() & 0xff) << 8;
  x |= (u.rawbyte() & 0xff) << 16;
  x |= (u.rawbyte() & 0xff) << 24;
  return u;
}

unmarshall &
operator>>(unmarshall &u, unsigned long long &x)
{
  unsigned int a, b;
  u >> a;
  u >> b;
  x = a | ((unsigned long long) b << 32);
  return u;
}

unmarshall &
operator>>(unmarshall &u, std::string &s)
{
  unsigned sz;
  u >> sz;
  if(u.ok())
    s = u.rawbytes(sz);
  return u;
}

std::string
unmarshall::rawbytes(unsigned int n)
{
  char *p = new char [n];
  unsigned nn = s.readsome(p, n);
  if(nn < n)
    _ok = false;
  std::string s(p, n);
  delete [] p;
  return s;
}

unsigned int
unmarshall::i32()
{
  unsigned int x;
  (*this) >> x;
  return x;
}

unsigned long long
unmarshall::i64()
{
  unsigned long long x;
  (*this) >> x;
  return x;
}

std::string
unmarshall::istr()
{
  std::string s;
  (*this) >> s;
  return s;
}

void 
make_sockaddr(const char *hostandport, struct sockaddr_in *dst){

  char host[200];
  const char *localhost = "127.0.0.1";
  const char *port = index(hostandport, ':');
  if(port == NULL){
    memcpy(host, localhost, strlen(localhost)+1);
    port = hostandport;
  }else{
    memcpy(host, hostandport, port-hostandport);
    host[port-hostandport] = '\0';
    port++;
  }

  make_sockaddr(host, port, dst);

}

void 
make_sockaddr(const char *host, const char *port, struct sockaddr_in *dst){

  in_addr_t a;

  bzero(dst, sizeof(*dst));
  dst->sin_family = AF_INET;

  a = inet_addr(host);
  if(a != INADDR_NONE){
    dst->sin_addr.s_addr = a;
  } else {
    struct hostent *hp = gethostbyname(host);
    if(hp == 0 || hp->h_length != 4){
      fprintf(stderr, "cannot find host name %s\n", host);
      exit(1);
    }
    dst->sin_addr.s_addr = ((struct in_addr *)(hp->h_addr))->s_addr;
  }
  dst->sin_port = htons(atoi(port));
}
