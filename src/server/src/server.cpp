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
#include "stinger_core/stinger.h"
#include "stinger_core/stinger_shared.h"
#include "stinger_core/xmalloc.h"
#include "stinger_utils/stinger_utils.h"
#include "stinger_utils/timer.h"
#include "stinger_utils/dimacs_support.h"
#include "stinger_utils/metisish_support.h"
#include "stinger_utils/json_support.h"
#include "stinger_utils/csv.h"
}

//#define LOG_AT_D
#include "stinger_core/stinger_error.h"

using namespace gt::stinger;


static char * graph_name = NULL;
static char * input_file = NULL;
static char * file_type = NULL;
static bool save_to_disk = false;
static char * stinger_config_file = NULL;

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

  graph_name = (char *) xmalloc (128*sizeof(char));
  sprintf(graph_name, "/stinger-default");
  input_file = (char *) xmalloc (1024*sizeof(char));
  input_file[0] = '\0';
  file_type = (char *) xmalloc (128*sizeof(char));
  file_type[0] = '\0';
  stinger_config_file = (char *) xmalloc(1024*sizeof(char));
  stinger_config_file[0] = '\0';
  int use_numerics = 0;

  /* parse command line configuration */
  int opt = 0;
  while(-1 != (opt = getopt(argc, argv, "?h"))) {
    switch(opt) {
      case '?':
      case 'h': {
		  printf("Usage:    %s\n"
       "   [-C Stinger Config file]\n"
			 "   [-a port_algs]\n"
			 "   [-s port_streams]\n"
			 "   [-n graph_name]\n"
			 "   [-i input_file_path]\n"
			 "   [-t file_type]\n"
			 "   [-1 (for numeric IDs)]\n"
			 "   [-d daemon mode]\n"
			 "   [-k write algorithm states to disk]\n"
			 "   [-v write vertex name mapping to disk]\n"
			 "   [-f output directory for vertex names, alg states]\n"
			 "   [-c cap number of history files to keep per alg]  \n", argv[0]);
		  printf("Defaults:\n\tport_algs: %d\n\tport_streams: %d\n\tgraph_name: %s\n", port_algs, port_streams, graph_name);
		  exit(0);
		} break;

    }
  }

  struct stinger_config_t * stinger_config = (struct stinger_config_t *)xcalloc(1,sizeof(struct stinger_config_t));

  libconfig::Config cfg;

  /* print configuration to the terminal */
  LOG_I_A("Name: %s", graph_name);
#ifdef __APPLE__
  master_tid = syscall(SYS_thread_selfid);
#else
  master_tid = syscall(SYS_gettid);
#endif

  /* allocate the graph */
  tic();
  struct stinger * S = stinger_shared_new_full(&graph_name, stinger_config);

  size_t graph_sz = S->length + sizeof(struct stinger);
  LOG_V_A("Data structure allocation time: %lf seconds", toc());

  xfree(stinger_config);


  LOG_V("Graph created. Running stats...");
  tic();
  LOG_V_A("Vertices: %ld", stinger_num_active_vertices(S));
  LOG_V_A("Edges: %ld", stinger_total_edges(S));

  /* consistency check */
  LOG_V_A("Consistency %ld", (long) stinger_consistency_check(S, S->max_nv));
  LOG_V_A("Done. %lf seconds", toc());

  /* initialize the singleton members */
  server_state.set_stinger(S);
  server_state.set_stinger_loc(graph_name);
  server_state.set_stinger_sz(graph_sz);
  server_state.set_port(port_streams, port_algs);
  server_state.set_mon_stinger(graph_name, sizeof(stinger_t) + S->length);

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

    struct stinger * S = server_state.get_stinger();
    size_t graph_sz = S->length + sizeof(struct stinger);
    
    LOG_V_A("Consistency %ld", (long) stinger_consistency_check(S, S->max_nv));

    /* snapshot to disk */
    if (save_to_disk) {
      int64_t rtn = stinger_save_to_file(S, stinger_max_active_vertex(S) + 1, input_file);
      LOG_D_A("save_to_file return code: %ld",rtn);
    }

    /* clean up */
    stinger_shared_free(S, graph_name, graph_sz);
    shmunlink(graph_name);
    free(graph_name);
    free(input_file);
    free(file_type);

#ifndef STINGER_USE_TCP
    /* Clean up unix sockets, which were created when the batch/alg server started up */
    char socket_path[128];
    snprintf(socket_path, sizeof(socket_path)-1, "/tmp/stinger.sock.%i", server_state.get_port_algs());
    unlink(socket_path);
    snprintf(socket_path, sizeof(socket_path)-1, "/tmp/stinger.sock.%i", server_state.get_port_streams());
    unlink(socket_path);
#endif

    /* clean up algorithm data stores */
    for (size_t i = 0; i < server_state.get_num_algs(); i++) {
      StingerAlgState * alg_state = server_state.get_alg(i);
      const char * alg_data_loc = alg_state->data_loc.c_str();
      shmunlink(alg_data_loc);
    }
  }
}

void
sigterm_cleanup (int)
{
  cleanup ();
  exit (EXIT_SUCCESS);
}
