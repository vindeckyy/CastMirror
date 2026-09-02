#include <gtest/gtest.h>
#include "castcore/device_auth.h"
#include "castcore/config.h"

#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace castcore;

class DeviceAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {}
};

TEST_F(DeviceAuthTest, RootCertificatesParseCorrectly) {
  size_t cast_len = 0;
  const uint8_t* cast_der = DeviceAuth::GetCastRootCaDer(&cast_len);
  ASSERT_NE(cast_der, nullptr);
  ASSERT_GT(cast_len, 0u);

  const unsigned char* p = cast_der;
  X509* cast_cert = d2i_X509(nullptr, &p, static_cast<long>(cast_len));
  ASSERT_NE(cast_cert, nullptr);

  char buf[256] = {0};
  X509_NAME_oneline(X509_get_subject_name(cast_cert), buf, sizeof(buf) - 1);
  std::string cast_subj(buf);
  EXPECT_NE(cast_subj.find("Cast Root CA"), std::string::npos);
  X509_free(cast_cert);

  size_t eureka_len = 0;
  const uint8_t* eureka_der = DeviceAuth::GetEurekaRootCaDer(&eureka_len);
  ASSERT_NE(eureka_der, nullptr);
  ASSERT_GT(eureka_len, 0u);

  p = eureka_der;
  X509* eureka_cert = d2i_X509(nullptr, &p, static_cast<long>(eureka_len));
  ASSERT_NE(eureka_cert, nullptr);

  memset(buf, 0, sizeof(buf));
  X509_NAME_oneline(X509_get_subject_name(eureka_cert), buf, sizeof(buf) - 1);
  std::string eureka_subj(buf);
  EXPECT_NE(eureka_subj.find("Eureka Root CA"), std::string::npos);
  X509_free(eureka_cert);
}

TEST_F(DeviceAuthTest, CastRootStoreInitializes) {
  X509_STORE* store = DeviceAuth::GetCastRootStore();
  ASSERT_NE(store, nullptr);
}

TEST_F(DeviceAuthTest, RejectsSelfSignedCertNotAnchoredInCastRoot) {
  // Generate a standalone self-signed certificate (like an untrusted attacker or fake receiver)
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  ASSERT_NE(pkey, nullptr);

  X509* cert = X509_new();
  ASSERT_NE(cert, nullptr);

  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 365 * 86400);
  X509_set_pubkey(cert, pkey);

  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"Fake Receiver", -1, -1, 0);
  X509_set_issuer_name(cert, name);

  ASSERT_GT(X509_sign(cert, pkey, EVP_sha256()), 0);

  // Verification against Cast Root CA MUST fail
  DeviceAuthResult res = DeviceAuth::VerifyPeerCertificate(cert, nullptr);
  EXPECT_FALSE(res.verified);
  EXPECT_FALSE(res.error_message.empty());
  EXPECT_NE(res.error_message.find("failed"), std::string::npos);

  X509_free(cert);
  EVP_PKEY_free(pkey);
}

TEST_F(DeviceAuthTest, NullPeerCertificateHandledGracefully) {
  DeviceAuthResult res = DeviceAuth::VerifyPeerCertificate(nullptr, nullptr);
  EXPECT_FALSE(res.verified);
  EXPECT_FALSE(res.error_message.empty());
}

TEST_F(DeviceAuthTest, ConfigureSslContextEnablesPeerVerify) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  ASSERT_NE(ctx, nullptr);

  // Enabled
  EXPECT_TRUE(DeviceAuth::ConfigureSslContext(ctx, true));
  EXPECT_EQ(SSL_CTX_get_verify_mode(ctx), SSL_VERIFY_PEER);

  // Escape hatch disabled
  EXPECT_TRUE(DeviceAuth::ConfigureSslContext(ctx, false));
  EXPECT_EQ(SSL_CTX_get_verify_mode(ctx), SSL_VERIFY_NONE);

  SSL_CTX_free(ctx);
}

TEST_F(DeviceAuthTest, ConfigIncludesVerifyDeviceCertDefaultTrue) {
  AppConfig cfg;
  EXPECT_TRUE(cfg.verify_device_cert);

  ConfigStore& store = ConfigStore::Instance();
  store.Mutable().verify_device_cert = false;
  EXPECT_FALSE(store.Get().verify_device_cert);
  store.Mutable().verify_device_cert = true;
  EXPECT_TRUE(store.Get().verify_device_cert);
}

TEST_F(DeviceAuthTest, NonceGenerationProducesRequestedLength) {
  auto nonce = DeviceAuth::GenerateNonce(16);
  EXPECT_EQ(nonce.size(), 16u);

  auto nonce32 = DeviceAuth::GenerateNonce(32);
  EXPECT_EQ(nonce32.size(), 32u);
  EXPECT_NE(nonce, nonce32);
}
