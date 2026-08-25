#include <gtest/gtest.h>
#include "castcore/frame_crypto.h"
#include "castcore/mirroring_negotiator.h"

using namespace castcore;

TEST(CryptoTest, EncryptDecryptRoundtrip) {
  auto keys = MirroringNegotiator::GenerateRandomKeys();
  FrameCrypto crypto(keys.aes_key, keys.aes_iv_mask);

  std::vector<uint8_t> original_data(1024);
  for (size_t i = 0; i < original_data.size(); ++i) {
    original_data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  uint32_t frame_id = 42;
  std::vector<uint8_t> encrypted = crypto.Encrypt(frame_id, original_data);

  EXPECT_EQ(encrypted.size(), original_data.size());
  EXPECT_NE(encrypted, original_data);

  std::vector<uint8_t> decrypted = crypto.Decrypt(frame_id, encrypted);
  EXPECT_EQ(decrypted, original_data);
}

TEST(CryptoTest, DifferentFramesProduceDifferentCiphertext) {
  auto keys = MirroringNegotiator::GenerateRandomKeys();
  FrameCrypto crypto(keys.aes_key, keys.aes_iv_mask);

  std::vector<uint8_t> original_data(512, 0xAA);

  std::vector<uint8_t> enc1 = crypto.Encrypt(1, original_data);
  std::vector<uint8_t> enc2 = crypto.Encrypt(2, original_data);

  EXPECT_NE(enc1, enc2);
}
