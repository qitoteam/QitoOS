/*
 * QitoOS - TLS 1.2 client
 * Unblocks qtpkg against real HTTPS hosts. Needs AES-GCM, SHA-256, RSA/ECDHE, X.509 parsing.
 * Large, but difference between real package manager and demo.
 */

#ifndef QITO_TLS_H
#define QITO_TLS_H

#include <kernel/types.h>

#define TLS_VERSION_1_2 0x0303

struct tls_context {
    bool_t handshake_done;
    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t master_secret[48];
};

int  tls_init(void);
int  tls_connect(const char *host, int port, struct tls_context *ctx);
int  tls_send(struct tls_context *ctx, const void *data, size_t len);
int  tls_recv(struct tls_context *ctx, void *buffer, size_t max);
void tls_close(struct tls_context *ctx);

/* Crypto primitives (stubs for now, real implementations planned) */
int  aes_gcm_encrypt(const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                     const uint8_t *plaintext, size_t pt_len,
                     const uint8_t *aad, size_t aad_len,
                     uint8_t *ciphertext, uint8_t *tag, size_t tag_len);
int  aes_gcm_decrypt(const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                     const uint8_t *ciphertext, size_t ct_len,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *tag, size_t tag_len,
                     uint8_t *plaintext);
int  sha256(const uint8_t *data, size_t len, uint8_t out[32]);
int  rsa_verify(const uint8_t *pubkey, size_t pubkey_len, const uint8_t *msg, size_t msg_len,
                const uint8_t *sig, size_t sig_len);
int  ecdhe_generate_keypair(uint8_t *priv, uint8_t *pub, size_t len);
int  x509_parse(const uint8_t *data, size_t len);

#endif /* QITO_TLS_H */
