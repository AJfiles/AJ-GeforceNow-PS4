#ifndef DISABLE_PEER_SIGNALING
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
extern int ps4_secure_random(void* buffer, size_t size);
extern void opennow_write_stream_startup_stage_from_c(const char* stage);
extern void opennow_log_app_lifecycle_from_c(const char* event, const char* detail);

static int ps4_entropy_poll(void *ctx, unsigned char *out, size_t len, size_t *olen) {
  (void)ctx;
  if (olen == NULL || (len != 0 && out == NULL)) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  *olen = 0;
  while (len > 0) {
    size_t chunk = len > 256 ? 256 : len;
    if (ps4_secure_random(out, chunk) != 0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    out += chunk;
    len -= chunk;
    *olen += chunk;
  }
  return 0;
}

#include <sys/select.h>
#include "config.h"
#include "ports.h"
#include "ssl_transport.h"
#include "utils.h"

static int ssl_transport_mbedtls_recv_timeout(void* ctx, unsigned char* buf, size_t len, uint32_t timeout) {
  int ret = MBEDTLS_ERR_SSL_TIMEOUT;
  fd_set read_fds;
  struct timeval tv;
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;

  FD_ZERO(&read_fds);
  FD_SET(((TcpSocket*)ctx)->fd, &read_fds);

  ret = select(((TcpSocket*)ctx)->fd + 1, &read_fds, NULL, NULL, &tv);
  if (ret < 0) {
    return -1;
  } else if (ret == 0) {
    // timeout
  } else {
    if (FD_ISSET(((TcpSocket*)ctx)->fd, &read_fds)) {
      ret = tcp_socket_recv((TcpSocket*)ctx, buf, len);
    }
  }

  return ret;
}

static int ssl_transport_mbedlts_send(void* ctx, const uint8_t* buf, size_t len) {
  return tcp_socket_send((TcpSocket*)ctx, buf, len);
}

static const char* ssl_transport_ca_bundle = NULL;
static char ssl_transport_error[192];

const char* ssl_transport_last_error(void) {
  return ssl_transport_error;
}

int ssl_transport_set_ca_bundle(const char* pem_bundle) {
  if (pem_bundle == NULL || pem_bundle[0] == '\0') return -1;
  ssl_transport_ca_bundle = pem_bundle;
  return 0;
}

int ssl_transport_connect(NetworkContext_t* net_ctx,
                          const char* host,
                          uint16_t port,
                          const char* cacert) {
  const char* pers = "ssl_client";
  int ret;
  Address resolved_addr;
  const char* failed_stage = "argument_validation";

  ssl_transport_error[0] = '\0';

  if (net_ctx == NULL || host == NULL || host[0] == '\0') return -1;
  if (cacert == NULL) cacert = ssl_transport_ca_bundle;
  if (cacert == NULL || cacert[0] == '\0') {
    LOGE("TLS root CA bundle has not been configured");
    return -1;
  }

  memset(net_ctx, 0, sizeof(*net_ctx));
  net_ctx->tcp_socket.fd = -1;
  mbedtls_ssl_init(&net_ctx->ssl);
  mbedtls_ssl_config_init(&net_ctx->conf);
  mbedtls_x509_crt_init(&net_ctx->cacert);
  mbedtls_ctr_drbg_init(&net_ctx->ctr_drbg);
  mbedtls_entropy_init(&net_ctx->entropy);
  failed_stage = "entropy_source";
  opennow_write_stream_startup_stage_from_c("tls_entropy_source_begin");
  ret = mbedtls_entropy_add_source(&net_ctx->entropy, ps4_entropy_poll, NULL,
                                   MBEDTLS_ENTROPY_BLOCK_SIZE,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (ret != 0) goto fail;

  failed_stage = "random_generator_seed";
  opennow_write_stream_startup_stage_from_c("tls_random_seed_begin");
  if ((ret = mbedtls_ctr_drbg_seed(&net_ctx->ctr_drbg, mbedtls_entropy_func, &net_ctx->entropy,
                                   (const unsigned char*)pers, strlen(pers))) != 0) {
    goto fail;
  }

  failed_stage = "tls_configuration";
  opennow_write_stream_startup_stage_from_c("tls_config_begin");
  if ((ret = mbedtls_ssl_config_defaults(&net_ctx->conf,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    LOGE("ssl config error: -0x%x", (unsigned int)-ret);
    goto fail;
  }

  failed_stage = "ca_bundle_parse";
  opennow_write_stream_startup_stage_from_c("tls_ca_parse_begin");
  ret = mbedtls_x509_crt_parse(&net_ctx->cacert,
                               (const unsigned char*)cacert, strlen(cacert) + 1);
  if (ret < 0 || net_ctx->cacert.raw.p == NULL) {
    LOGE("TLS root CA bundle parse failed: -0x%x", (unsigned int)-ret);
    goto fail;
  }
  opennow_write_stream_startup_stage_from_c("tls_ca_parse_complete");
  mbedtls_ssl_conf_ca_chain(&net_ctx->conf, &net_ctx->cacert, NULL);
  mbedtls_ssl_conf_authmode(&net_ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

  mbedtls_ssl_conf_rng(&net_ctx->conf, mbedtls_ctr_drbg_random, &net_ctx->ctr_drbg);

  failed_stage = "ssl_context_setup";
  opennow_write_stream_startup_stage_from_c("tls_ssl_setup_begin");
  if ((ret = mbedtls_ssl_setup(&net_ctx->ssl, &net_ctx->conf)) != 0) {
    LOGE("ssl setup error: -0x%x", (unsigned int)-ret);
    goto fail;
  }

  failed_stage = "server_hostname_setup";
  if ((ret = mbedtls_ssl_set_hostname(&net_ctx->ssl, host)) != 0) {
    LOGE("ssl set hostname error: -0x%x", (unsigned int)-ret);
    goto fail;
  }

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  failed_stage = "tcp_socket_open";
  opennow_write_stream_startup_stage_from_c("tls_socket_open_begin");
  ret = tcp_socket_open(&net_ctx->tcp_socket, AF_INET);
  if (ret < 0) goto fail;
  failed_stage = "dns_resolve";
  opennow_write_stream_startup_stage_from_c("tls_dns_resolve_begin");
  ret = ports_resolve_addr(host, &resolved_addr);
  if (ret < 0) {
    opennow_write_stream_startup_stage_from_c("tls_dns_resolve_failed");
    goto fail;
  }
  opennow_write_stream_startup_stage_from_c("tls_dns_resolve_complete");
  addr_set_port(&resolved_addr, port);
  failed_stage = "tcp_connect";
  opennow_write_stream_startup_stage_from_c("tls_tcp_connect_begin");
  ret = tcp_socket_connect(&net_ctx->tcp_socket, &resolved_addr);
  if (ret < 0) {
    goto fail;
  }

  mbedtls_ssl_conf_read_timeout(&net_ctx->conf, CONFIG_TLS_READ_TIMEOUT);
  mbedtls_ssl_set_bio(&net_ctx->ssl, &net_ctx->tcp_socket,
                      ssl_transport_mbedlts_send, NULL, ssl_transport_mbedtls_recv_timeout);

  LOGI("start to handshake");

  failed_stage = "server_tls_handshake";
  opennow_write_stream_startup_stage_from_c("tls_handshake_begin");
  while ((ret = mbedtls_ssl_handshake(&net_ctx->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      LOGE("ssl handshake error: -0x%x", (unsigned int)-ret);
      goto fail;
    }
  }

  opennow_write_stream_startup_stage_from_c("tls_handshake_complete");
  failed_stage = "certificate_verification";
  opennow_write_stream_startup_stage_from_c("tls_verify_begin");
  ret = (int)mbedtls_ssl_get_verify_result(&net_ctx->ssl);
  if (ret != 0) {
    char detail[128];
    snprintf(detail, sizeof(detail), "stage=%s verify_flags=0x%08x", failed_stage, (unsigned)ret);
    opennow_log_app_lifecycle_from_c("TLS_CONNECT_FAILED", detail);
    opennow_write_stream_startup_stage_from_c("tls_certificate_verification_failed");
    LOGE("TLS server certificate verification failed");
    goto fail;
  }

  opennow_write_stream_startup_stage_from_c("tls_verify_complete");
  LOGI("handshake success");
  return 0;

fail:
  {
    char detail[160];
    snprintf(detail, sizeof(detail), "stage=%s mbedtls_rc=%d verify_flags=0x%08x",
             failed_stage, ret, (unsigned)mbedtls_ssl_get_verify_result(&net_ctx->ssl));
    snprintf(ssl_transport_error, sizeof(ssl_transport_error), "%s", detail);
    opennow_log_app_lifecycle_from_c("TLS_CONNECT_FAILED", detail);
    opennow_write_stream_startup_stage_from_c("tls_connect_failed");
  }
  ssl_transport_disconnect(net_ctx);
  return -1;
}

void ssl_transport_disconnect(NetworkContext_t* net_ctx) {
  mbedtls_ssl_config_free(&net_ctx->conf);
  mbedtls_x509_crt_free(&net_ctx->cacert);
  mbedtls_ctr_drbg_free(&net_ctx->ctr_drbg);
  mbedtls_entropy_free(&net_ctx->entropy);
  mbedtls_ssl_free(&net_ctx->ssl);

  tcp_socket_close(&net_ctx->tcp_socket);
}

int32_t ssl_transport_recv(NetworkContext_t* net_ctx, void* buf, size_t len) {
  int ret;
  memset(buf, 0, len);
  ret = mbedtls_ssl_read(&net_ctx->ssl, buf, len);

  return ret;
}

int32_t ssl_transport_send(NetworkContext_t* net_ctx, const void* buf, size_t len) {
  int ret;

  int retry_count = 0;
  const int max_retry = 5;
  while ((ret = mbedtls_ssl_write(&net_ctx->ssl, buf, len)) <= 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      LOGE("ssl write error: -0x%x", (unsigned int)-ret);
    }

    if (++retry_count >= max_retry) {
      LOGE("ssl write max retry reached");
      return -1;
    }
  }

  return ret;
}
#endif  // DISABLE_PEER_SIGNALING
