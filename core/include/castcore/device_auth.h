#ifndef CASTCORE_DEVICE_AUTH_H_
#define CASTCORE_DEVICE_AUTH_H_

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <openssl/x509.h>
#include <openssl/ssl.h>

namespace castcore {

struct DeviceAuthResult {
  bool verified = false;
  std::string error_message;
  std::string peer_cert_subject;
  std::string peer_cert_issuer;
  std::string common_name;
};

class DeviceAuth {
 public:
  // Returns singleton X509_STORE containing Google Cast Root CA and Eureka Root CA
  static X509_STORE* GetCastRootStore();

  // Create a fresh X509_STORE with Cast CAs
  static X509_STORE* CreateCastRootStore();

  // Verify an X509 certificate chain against Cast Root Store
  static DeviceAuthResult VerifyPeerCertificate(X509* peer_cert,
                                                STACK_OF(X509)* untrusted_chain = nullptr);

  // Custom verify callback for OpenSSL SSL_CTX_set_verify
  static int SslVerifyCallback(int preverify_ok, X509_STORE_CTX* ctx);

  // Configure SSL_CTX with Cast Root CA store and peer verification policy
  static bool ConfigureSslContext(SSL_CTX* ctx, bool verify_device_cert);

  // Verify a raw DER leaf certificate + untrusted intermediates against Cast Root Store
  static DeviceAuthResult VerifyDerCertificateChain(
      const std::vector<uint8_t>& leaf_der,
      const std::vector<std::vector<uint8_t>>& intermediates_der);

  // Generate a random 16-byte nonce for DeviceAuth challenge
  static std::vector<uint8_t> GenerateNonce(size_t length = 16);

  // Verify Cast DeviceAuth response signature and certificate chain
  static DeviceAuthResult VerifyAuthResponse(
      const std::vector<uint8_t>& leaf_cert_der,
      const std::vector<std::vector<uint8_t>>& intermediates_der,
      const std::vector<uint8_t>& signature,
      const std::vector<uint8_t>& nonce);

  // Raw DER constants accessors for testing
  static const uint8_t* GetCastRootCaDer(size_t* len);
  static const uint8_t* GetEurekaRootCaDer(size_t* len);
};

}  // namespace castcore

#endif  // CASTCORE_DEVICE_AUTH_H_
