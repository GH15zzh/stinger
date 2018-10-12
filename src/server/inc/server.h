#ifndef _SERVER_H
#define _SERVER_H

#include "stinger_net/proto/stinger-batch.pb.h"
#include "stinger_net/send_rcv.h"

using namespace gt::stinger;

/* function prototypes */
void * start_batch_server (void * args);

#endif  /* _SERVER_H */
