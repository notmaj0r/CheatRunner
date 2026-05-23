#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_client.h"

typedef struct http_ctx {
  int libnet_mem_id;
  int libssl_ctx_id;
  int libhttp_ctx_id;
  int tmpl_id;
  int conn_id;
  int req_id;
} http_ctx_t;

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int memid);

int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx_id);

int sceHttpInit(int memid, int ssl_ctx_id, size_t pool_size);
int sceHttpTerm(int ctx_id);
int sceHttpCreateTemplate(int libhttp_ctx_id, const char *agent, int http_ver, int auto_proxy);
int sceHttpsSetSslCallback(int tmpl_id, void *cb, void *user_arg);
int sceHttpSetResponseHeaderMaxSize(int tmpl_id, size_t size);
int sceHttpDeleteTemplate(int tmpl_id);
int sceHttpCreateConnectionWithURL(int tmpl_id, const char *url, int keep_alive);
int sceHttpDeleteConnection(int conn_id);
int sceHttpCreateRequestWithURL(int conn_id, int method, const char *url, uint64_t content_length);
int sceHttpSendRequest(int req_id, const void *post_data, size_t size);
int sceHttpSetConnectTimeOut(int id, unsigned int usec);
int sceHttpSetResolveTimeOut(int id, unsigned int usec);
int sceHttpSetSendTimeOut(int id, unsigned int usec);
int sceHttpSetRecvTimeOut(int id, unsigned int usec);
int sceHttpGetResponseContentLength(int req_id, int *known, uint64_t *content_length);
int sceHttpGetStatusCode(int req_id, int *status_code);
int sceHttpReadData(int req_id, void *data, size_t size);
int sceHttpDeleteRequest(int req_id);

static int
http_ssl_cb(void) {
  return 0;
}

static int
http_init(http_ctx_t *ctx, const char *agent, const char *url, int timeout_ms) {
  int err = 0;

  ctx->libnet_mem_id = -1;
  ctx->libssl_ctx_id = -1;
  ctx->libhttp_ctx_id = -1;
  ctx->tmpl_id = -1;
  ctx->conn_id = -1;
  ctx->req_id = -1;

  if ((err = sceNetInit()) < 0) {
    return err;
  }
  if ((ctx->libnet_mem_id = sceNetPoolCreate("cheatrunner_http", 16 * 1024, 0)) < 0) {
    return ctx->libnet_mem_id;
  }
  if ((ctx->libssl_ctx_id = sceSslInit(128 * 1024)) < 0) {
    return ctx->libssl_ctx_id;
  }
  if ((ctx->libhttp_ctx_id = sceHttpInit(ctx->libnet_mem_id, ctx->libssl_ctx_id, 32 * 1024)) < 0) {
    return ctx->libhttp_ctx_id;
  }
  if ((ctx->tmpl_id = sceHttpCreateTemplate(ctx->libhttp_ctx_id, agent, 2, 1)) < 0) {
    return ctx->tmpl_id;
  }
  if ((err = sceHttpSetResponseHeaderMaxSize(ctx->tmpl_id, 0x2000)) < 0) {
    return err;
  }
  if ((err = sceHttpsSetSslCallback(ctx->tmpl_id, http_ssl_cb, 0)) < 0) {
    return err;
  }
  if ((ctx->conn_id = sceHttpCreateConnectionWithURL(ctx->tmpl_id, url, 0)) < 0) {
    return ctx->conn_id;
  }
  if ((ctx->req_id = sceHttpCreateRequestWithURL(ctx->conn_id, 0, url, 0)) < 0) {
    return ctx->req_id;
  }
  if (timeout_ms > 0) {
    unsigned int t_us = (unsigned int)timeout_ms * 1000u;
    (void)sceHttpSetConnectTimeOut(ctx->req_id, t_us);
    (void)sceHttpSetResolveTimeOut(ctx->req_id, t_us);
    (void)sceHttpSetSendTimeOut(ctx->req_id, t_us);
    (void)sceHttpSetRecvTimeOut(ctx->req_id, t_us);
  }

  return 0;
}

static int
http_request(http_ctx_t *ctx, uint8_t **data, size_t *len, int *status_out, size_t max_bytes) {
  int err = 0;
  int status = -1;

  *data = NULL;
  *len = 0;
  if (status_out) {
    *status_out = -1;
  }

  if ((err = sceHttpSendRequest(ctx->req_id, 0, 0)) < 0) {
    return err;
  }
  if ((err = sceHttpGetStatusCode(ctx->req_id, &status)) < 0) {
    return err;
  }
  if (status_out) {
    *status_out = status;
  }

  int cl_known = 1;
  uint64_t probed = 0;
  if (sceHttpGetResponseContentLength(ctx->req_id, &cl_known, &probed) < 0) {
    cl_known = 1;
    probed = 0;
  }
  if (max_bytes > 0 && cl_known == 0 && probed > (uint64_t)max_bytes) {
    return -2;
  }

  size_t cap = (cl_known == 0 && probed > 0) ? (size_t)probed : 16384;
  if (max_bytes > 0 && cap > max_bytes) {
    cap = max_bytes;
    if (cap == 0) {
      cap = 1;
    }
  }
  size_t off = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) {
    return -1;
  }

  for (;;) {
    if ((cap - off) < 4096) {
      size_t new_cap = cap * 2;
      uint8_t *nbuf = realloc(buf, new_cap);
      if (!nbuf) {
        free(buf);
        return -1;
      }
      buf = nbuf;
      cap = new_cap;
    }

    int n = sceHttpReadData(ctx->req_id, buf + off, cap - off);
    if (n < 0) {
      free(buf);
      return n;
    }
    if (n == 0) {
      break;
    }
    if (max_bytes > 0 && off + (size_t)n > max_bytes) {
      free(buf);
      return -2;
    }
    off += (size_t)n;
  }

  *data = buf;
  *len = off;
  return 0;
}

static void
http_fini(http_ctx_t *ctx) {
  if (ctx->req_id >= 0) {
    sceHttpDeleteRequest(ctx->req_id);
  }
  if (ctx->conn_id >= 0) {
    sceHttpDeleteConnection(ctx->conn_id);
  }
  if (ctx->tmpl_id >= 0) {
    sceHttpDeleteTemplate(ctx->tmpl_id);
  }
  if (ctx->libhttp_ctx_id >= 0) {
    sceHttpTerm(ctx->libhttp_ctx_id);
  }
  if (ctx->libssl_ctx_id >= 0) {
    sceSslTerm(ctx->libssl_ctx_id);
  }
  if (ctx->libnet_mem_id >= 0) {
    sceNetPoolDestroy(ctx->libnet_mem_id);
  }
}

uint8_t *
http_get_url(const char *agent, const char *url, size_t *len) {
  int status = 0;
  uint8_t *body = NULL;
  size_t body_len = 0;
  if (http_get_url_ex(agent, url, 0, &status, &body, &body_len) != 0 || status != 200) {
    free(body);
    return NULL;
  }
  if (len) {
    *len = body_len;
  }
  return body;
}

int
http_get_url_ex(const char *agent, const char *url, size_t max_bytes,
                int *status_out, uint8_t **data_out, size_t *len_out) {
  return http_get_url_ex_timeout(agent, url, max_bytes, 0, status_out, data_out, len_out);
}

int
http_get_url_ex_timeout(const char *agent, const char *url, size_t max_bytes, int timeout_ms,
                        int *status_out, uint8_t **data_out, size_t *len_out) {
  http_ctx_t ctx;
  uint8_t *body = NULL;
  size_t body_len = 0;
  int status = -1;
  int rc = 0;

  if (!url || !data_out || !len_out) {
    return -1;
  }
  *data_out = NULL;
  *len_out = 0;
  if (status_out) {
    *status_out = -1;
  }

  if (!agent) {
    agent = "CheatRunner/0.1";
  }

  if (http_init(&ctx, agent, url, timeout_ms) < 0) {
    http_fini(&ctx);
    return -1;
  }

  rc = http_request(&ctx, &body, &body_len, &status, max_bytes);
  if (status_out) {
    *status_out = status;
  }
  http_fini(&ctx);
  if (rc != 0) {
    free(body);
    return rc;
  }
  *data_out = body;
  *len_out = body_len;
  return 0;
}
