#include <cstdio>
#include <algorithm>
#include <limits>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <libconfig.h++>

#include "server.h"
#include "stinger_net/stinger_server_state.h"

extern "C" {
#include "stinger_core/stinger_shared.h"
#include "stinger_core/xmalloc.h"
#include "stinger_utils/stinger_utils.h"
#include "stinger_utils/timer.h"
#include "stinger_utils/dimacs_support.h"
#include "stinger_utils/metisish_support.h"
#include "stinger_utils/csv.h"
}

//#define LOG_AT_D
#include "stinger_core/stinger_error.h"

static int start_pipe[2] = {-1, -1};

pid_t master_tid;
static pthread_t batch_server_tid, alg_server_tid;

static StingerServerState & server_state = StingerServerState::get_server_state();

static void cleanup (void);
extern "C" {
  static void sigterm_cleanup (int);
}
  
int main(int argc, char *argv[])
{
  /* default global options */
  int port_streams = 10102;
  int port_algs = 10103;
  int unleash_daemon = 0;

  /* parse command line configuration */
  int opt = 0;
  while(-1 != (opt = getopt(argc, argv, "?h"))) {
    switch(opt) {
      case '?':
      case 'h': {
		  printf("Usage:    %s\n"
			 "   [-c cap number of history files to keep per alg]  \n", argv[0]);
		  printf("Defaults:\n\tport_algs: %d\n\tport_streams: %d\n\tgraph_name:\n", port_algs, port_streams);
		  exit(0);
		} break;

    }
  }

#ifdef __APPLE__
  master_tid = syscall(SYS_thread_selfid);
#else
  master_tid = syscall(SYS_gettid);
#endif

  server_state.set_port(port_streams, port_algs);

  /* this thread will handle the batch & alg servers */
  /* TODO: bring the thread creation for the alg server to this level */
  pthread_create(&batch_server_tid, NULL, start_batch_server, NULL);

  {
    /* Inform the parent that we're ready for connections. */
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset (&sa.sa_mask);
    sa.sa_handler = sigterm_cleanup;
    /* Ignore the old handlers. */
    sigaction (SIGINT, &sa, NULL);
    sigaction (SIGTERM, &sa, NULL);
    sigaction (SIGHUP, &sa, NULL);
  }

  if(unleash_daemon) {
    int exitcode = EXIT_SUCCESS;
    write (start_pipe[1], &exitcode, sizeof (exitcode));
    close (start_pipe[1]);
    while(1) { sleep(10); }
  } else {
    LOG_I("Press Ctrl-C to shut down the server...");
    while(1) { sleep(10); }
  }

  return 0;
}

void
cleanup (void)
{
  pid_t tid;
#ifdef __APPLE__
  tid = syscall(SYS_thread_selfid);
#else
  tid = syscall(SYS_gettid);
#endif
  /* Only the main thread executes */
  if (tid == master_tid) {
    LOG_I("Shutting down the batch server..."); fflush(stdout);
    pthread_cancel(batch_server_tid);
    pthread_join(batch_server_tid, NULL);
    LOG_I("done."); fflush(stdout);

    LOG_I("Shutting down the alg server..."); fflush(stdout);
    pthread_cancel(alg_server_tid);
    pthread_join(alg_server_tid, NULL);
    LOG_I("done."); fflush(stdout);

#ifndef STINGER_USE_TCP
    /* Clean up unix sockets, which were created when the batch/alg server started up */
    char socket_path[128];
    snprintf(socket_path, sizeof(socket_path)-1, "/tmp/stinger.sock.%i", server_state.get_port_algs());
    unlink(socket_path);
    snprintf(socket_path, sizeof(socket_path)-1, "/tmp/stinger.sock.%i", server_state.get_port_streams());
    unlink(socket_path);
#endif
  }
}

void
sigterm_cleanup (int)
{
  cleanup ();
  exit (EXIT_SUCCESS);
}
