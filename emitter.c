#include <errno.h>
#include <stdint.h> /* C99 for int64_t */
#include <string.h>
#include <unistd.h>
#include "zmq.h"
#include "ring.h"
#include "emitter.h"
#include "insist.h"
#include "proto.h"
#include "backoff.h"
#include "clock_gettime.h"
#include "flog.h"

#include <sys/resource.h>

#include "sleepdefs.h"

void *emitter(void *arg) {
  struct emitter_config *config = arg;
  int rc;

  void *socket = zmq_socket(config->zmq, ZMQ_PULL);
  insist(socket != NULL, "zmq_socket() failed: %s", strerror(errno));
  int64_t hwm = 100;
  zmq_setsockopt(socket, ZMQ_HWM, &hwm, sizeof(hwm));
  rc = zmq_bind(socket, config->zmq_endpoint);
  insist(rc != -1, "zmq_bind(%s) failed: %s", config->zmq_endpoint,
         zmq_strerror(errno));

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  //long count = 0;

  struct backoff sleeper;
  backoff_init(&sleeper, &MIN_SLEEP, &MAX_SLEEP);

  struct lumberjack *lumberjack;
  lumberjack = lumberjack_new(config->host, config->port, config->window_size);
  insist(lumberjack != NULL, "lumberjack_new failed");
  lumberjack->ring_size = config->window_size;

  if (config->ssl_ca_path != NULL) {
    rc = lumberjack_set_ssl_ca(lumberjack, config->ssl_ca_path);
    insist(rc == 0, "lumberjack_set_ssl_ca failed, is '%s' a valid ssl cert?",
           config->ssl_ca_path);
  }

  long count = 0;
  long bytes = 0;

  zmq_pollitem_t items[1];

  items[0].socket = socket;
  items[0].events = ZMQ_POLLIN;

  for (;;) {
    /* Receive an event from a harvester and put it in the queue */
    zmq_msg_t message;

    rc = zmq_msg_init(&message);
    insist(rc == 0, "zmq_msg_init failed");
    rc = zmq_poll(items, 1, 1000000 /* microseconds */);

    if (rc == 0) {
      /* poll timeout. We're idle, so let's flush and back-off. */
      //if (rc != 0 && errno == EAGAIN) {
      flog(stdout, "flushing since nothing came in over zmq");
      /* We flush here to keep slow feeders closer to real-time */
      rc = lumberjack_flush(lumberjack);
      if (rc != 0) {
        /* write failure, reconnect (which will resend) and such */
        lumberjack_disconnect(lumberjack);
        lumberjack_ensure_connected(lumberjack);
      }
      backoff(&sleeper);
      continue;
    } 

    /* poll successful, read a message */
    rc = zmq_recv(socket, &message, 0);
    insist(rc == 0 /*|| errno == EAGAIN */,
           "zmq_recv(%s) failed (returned %d): %s",
           config->zmq_endpoint, rc, zmq_strerror(errno));

    /* Clear the backoff timer since we received a message successfully */
    backoff_clear(&sleeper);

    /* Write the data over lumberjack. This will handle any
     * connection/reconnection/ack issues */
    lumberjack_send_data(lumberjack, zmq_msg_data(&message),
                         zmq_msg_size(&message));
    /* Stats for debugging */
    count++;
    bytes += zmq_msg_size(&message);

    zmq_msg_close(&message);

    if (count == 10000) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      double s = (start.tv_sec + 0.0) + (start.tv_nsec / 1000000000.0);
      double n = (now.tv_sec + 0.0) + (now.tv_nsec / 1000000000.0);
      fprintf(stderr, "Rate: %f (bytes: %f)\n", (count + 0.0) / (n - s), (bytes + 0.0) / (n - s));
      struct rusage rusage;
      rc = getrusage(RUSAGE_SELF, &rusage);
      insist(rc == 0, "getrusage failed: %s\n", strerror(errno));
      printf("cpu user/system: %d.%06d / %d.%06d\n",
             (int)rusage.ru_utime.tv_sec, (int)rusage.ru_utime.tv_usec,
             (int)rusage.ru_stime.tv_sec, (int)rusage.ru_stime.tv_usec);
      clock_gettime(CLOCK_MONOTONIC, &start);
      bytes = 0;
      count = 0;
    }
  } /* forever */
} /* emitter */

