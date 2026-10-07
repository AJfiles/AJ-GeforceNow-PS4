#include <srtp2/srtp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "peer.h"
#include "sctp.h"
#include "utils.h"

extern void opennow_write_stream_startup_stage_from_c(const char *stage);

static pthread_mutex_t peer_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned peer_runtime_users;

int peer_init() {
  opennow_write_stream_startup_stage_from_c("peer_init_lock_begin");
  pthread_mutex_lock(&peer_runtime_mutex);
  opennow_write_stream_startup_stage_from_c("peer_init_lock_acquired");
  if (peer_runtime_users == 0 && srtp_init() != srtp_err_status_ok) {
    opennow_write_stream_startup_stage_from_c("libsrtp_init_failed");
    pthread_mutex_unlock(&peer_runtime_mutex);
    LOGE("libsrtp init failed");
    return -1;
  }
  opennow_write_stream_startup_stage_from_c("libsrtp_init_complete");
  opennow_write_stream_startup_stage_from_c("sctp_runtime_init_begin");
  sctp_usrsctp_init();
  opennow_write_stream_startup_stage_from_c("sctp_runtime_init_complete");
  peer_runtime_users++;
  pthread_mutex_unlock(&peer_runtime_mutex);
  opennow_write_stream_startup_stage_from_c("peer_init_complete");
  return 0;
}

void peer_deinit() {
  pthread_mutex_lock(&peer_runtime_mutex);
  if (peer_runtime_users == 0) {
    pthread_mutex_unlock(&peer_runtime_mutex);
    return;
  }
  sctp_usrsctp_deinit();
  if (--peer_runtime_users == 0)
    srtp_shutdown();
  pthread_mutex_unlock(&peer_runtime_mutex);
}
