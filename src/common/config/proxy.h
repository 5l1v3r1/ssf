#ifndef SSF_COMMON_CONFIG_PROXY_H_
#define SSF_COMMON_CONFIG_PROXY_H_

#include <string>

#include <json.hpp>

#include "ssf/network/socks/socks.h"

namespace ssf {
namespace config {

class HttpProxy {
 public:
  using Json = nlohmann::json;

 public:
  HttpProxy();

 public:
  void Update(const Json& json);

  void Log() const;

  inline bool IsSet() const { return !host_.empty() && !port_.empty(); }

  inline std::string host() const { return host_; }

  inline std::string port() const { return port_; }

  inline std::string username() const { return username_; }

  inline std::string domain() const { return domain_; }

  inline std::string password() const { return password_; }

  inline std::string user_agent() const { return user_agent_; }

  inline bool reuse_ntlm() const { return reuse_ntlm_; }

  inline bool reuse_kerb() const { return reuse_kerb_; }

 private:
  // Proxy host
  std::string host_;
  // Proxy port
  std::string port_;
  // Custom user agent
  std::string user_agent_;
  // Credentials username
  std::string username_;
  // Credentials user's domain
  std::string domain_;
  // Credentials password
  std::string password_;
  // Reuse default NTLM credentials
  bool reuse_ntlm_;
  // Reuse default Kerberos/Negotiate credentials
  bool reuse_kerb_;
};

class SocksProxy {
 public:
  using Json = nlohmann::json;
  using Socks = ssf::network::Socks;

 public:
  SocksProxy();

 public:
  void Update(const Json& json);

  void Log() const;

  inline bool IsSet() const {
    return version_ != Socks::Version::kVUnknown && !host_.empty() &&
           !port_.empty();
  }

  inline Socks::Version version() const { return version_; }

  inline std::string host() const { return host_; }

  inline std::string port() const { return port_; }

 private:
  // Socks server version
  Socks::Version version_;
  // Proxy host
  std::string host_;
  // Proxy port
  std::string port_;
};

}  // config
}  // ssf

#endif  // SSF_COMMON_CONFIG_PROXY_H_