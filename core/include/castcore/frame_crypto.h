#ifndef CASTCORE_FRAME_CRYPTO_H_
#define CASTCORE_FRAME_CRYPTO_H_

#include "castcore/types.h"
#include <array>
#include <vector>

namespace castcore {

class FrameCrypto {
 public:
  FrameCrypto(const std::array<uint8_t, 16>& aes_key,
              const std::array<uint8_t, 16>& cast_iv_mask);
  ~FrameCrypto();

  // Encrypts payload in-place or into out buffer
  void Encrypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const;
  void Decrypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const;

  std::vector<uint8_t> Encrypt(uint32_t frame_id, const std::vector<uint8_t>& plain_data) const;
  std::vector<uint8_t> Decrypt(uint32_t frame_id, const std::vector<uint8_t>& cipher_data) const;

 private:
  void Crypt(uint32_t frame_id, const uint8_t* in, size_t in_len, uint8_t* out) const;

  std::array<uint8_t, 16> aes_key_{};
  std::array<uint8_t, 16> cast_iv_mask_{};
};

} // namespace castcore

#endif // CASTCORE_FRAME_CRYPTO_H_
