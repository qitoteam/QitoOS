/*
 * QitoOS - Ed25519 signatures for package integrity
 */

#ifndef QITO_ED25519_H
#define QITO_ED25519_H

#include <kernel/types.h>

#define ED25519_PUBLIC_KEY_SIZE 32
#define ED25519_PRIVATE_KEY_SIZE 64
#define ED25519_SIGNATURE_SIZE 64

int ed25519_verify(const uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key);
int ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key, const uint8_t *private_key);
void ed25519_create_keypair(uint8_t *public_key, uint8_t *private_key, const uint8_t *seed);

#endif
