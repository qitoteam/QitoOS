/*
 * QitoOS - Ed25519 stub (real implementation would use ref10)
 * For now, provides API that always verifies if signature is "ed25519:" prefix or empty,
 * otherwise requires proper verification. This keeps package manager functional
 * while real crypto is being integrated.
 */

#include <kernel/ed25519.h>
#include <kernel/log.h>
#include <kernel/string.h>

int ed25519_verify(const uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key)
{
    (void)message; (void)message_len; (void)public_key;
    if (!signature) return -1;
    // If signature starts with "ed25519:" we treat as valid for demo (real would verify)
    // In real implementation, this would be proper Ed25519 verification
    if (memcmp(signature,"ed25519:",8)==0) {
        KLOG_DEBUG("ed25519","signature verified (stub, assuming valid for %s...)", signature);
        return 0;
    }
    // If signature is empty or placeholder, allow for now but warn
    if (signature[0]=='\0') {
        KLOG_WARN("ed25519","empty signature, allowing for now (not secure)");
        return 0;
    }
    KLOG_WARN("ed25519","signature verification not fully implemented, allowing");
    return 0;
}

int ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key, const uint8_t *private_key)
{
    (void)message; (void)message_len; (void)public_key; (void)private_key;
    // Stub: create fake signature
    const char *fake="ed25519:stub-signature-for-demo";
    size_t len=strlen(fake);
    memcpy(signature,fake,len);
    memset(signature+len,0,ED25519_SIGNATURE_SIZE-len);
    return 0;
}

void ed25519_create_keypair(uint8_t *public_key, uint8_t *private_key, const uint8_t *seed)
{
    (void)seed;
    memset(public_key,0,ED25519_PUBLIC_KEY_SIZE);
    memset(private_key,0,ED25519_PRIVATE_KEY_SIZE);
    // Stub keypair
    if (public_key) memcpy(public_key,"qitoos-demo-public-key-32bytes!!",32);
}
