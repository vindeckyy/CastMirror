#include "castcore/frame_crypto.h"
#include "castcore/logger.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <cstring>
#include <limits>

namespace castcore {

FrameCrypto::FrameCrypto(const std::array<uint8_t, 16>& aes_key,
                         const std::array<uint8_t, 16>& cast_iv_mask)
    : aes_key_(aes_key), cast_iv_mask_(cast_iv_mask) {}

FrameCrypto::~FrameCrypto() {
  OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
  OPENSSL_cleanse(cast_iv_mask_.data(), cast_iv_mask_.size());
}

void FrameCrypto::Crypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const {
  if (in_len == 0 || !in || !out) return;
  if (in_len > static_cast<size_t>(std::numeric_limits<int>::max())) {
    LOG_ERROR << "FrameCrypto::Crypt payload exceeds INT_MAX";
    return;
  }

  std::array<uint8_t, 16> aes_nonce{};
  // Cast Streaming AES nonce calculation: frame_id lower 32 bits at offset 8 (big-endian)
  aes_nonce[8] = static_cast<uint8_t>((frame_id >> 24) & 0xFF);
  aes_nonce[9] = static_cast<uint8_t>((frame_id >> 16) & 0xFF);
  aes_nonce[10] = static_cast<uint8_t>((frame_id >> 8) & 0xFF);
  aes_nonce[11] = static_cast<uint8_t>(frame_id & 0xFF);

  for (size_t i = 0; i < 16; ++i) {
    aes_nonce[i] ^= cast_iv_mask_[i];
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    LOG_ERROR << "FrameCrypto::Crypt failed to allocate EVP_CIPHER_CTX";
    return;
  }

  int out_len = 0;
  int final_len = 0;
  const bool ok =
      EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, aes_key_.data(), aes_nonce.data()) == 1 &&
      EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
      EVP_EncryptUpdate(ctx, out, &out_len, in, static_cast<int>(in_len)) == 1 &&
      EVP_EncryptFinal_ex(ctx, out + out_len, &final_len) == 1;
  if (!ok) {
    LOG_ERROR << "FrameCrypto::Crypt AES-CTR failure for frame " << frame_id;
  }

  OPENSSL_cleanse(aes_nonce.data(), aes_nonce.size());
  EVP_CIPHER_CTX_free(ctx);
}

void FrameCrypto::Encrypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const {
  Crypt(frame_id, in, in_len, out);
}

void FrameCrypto::Decrypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const {
  Crypt(frame_id, in, in_len, out);
}

std::vector<uint8_t> FrameCrypto::Encrypt(uint32_t frame_id,
                                          const std::vector<uint8_t>& plain_data) const {
  std::vector<uint8_t> result(plain_data.size());
  Encrypt(frame_id, plain_data.data(), plain_data.size(), result.data());
  return result;
}

std::vector<uint8_t> FrameCrypto::Decrypt(uint32_t frame_id,
                                          const std::vector<uint8_t>& cipher_data) const {
  std::vector<uint8_t> result(cipher_data.size());
  Decrypt(frame_id, cipher_data.data(), cipher_data.size(), result.data());
  return result;
}

}  // namespace castcore
