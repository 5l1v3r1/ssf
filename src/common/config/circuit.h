#ifndef SSF_COMMON_CONFIG_CIRCUIT_H_
#define SSF_COMMON_CONFIG_CIRCUIT_H_

#include <list>
#include <string>

#include <json.hpp>

namespace ssf {
namespace config {

class CircuitNode {
 public:
  CircuitNode(const std::string& addr, const std::string& port);

 public:
  inline std::string addr() const { return addr_; }
  inline void set_addr(const std::string& addr) { addr_ = addr; }

  inline std::string port() const { return port_; }
  inline void set_port(const std::string& port) { port_ = port; }

 private:
  std::string addr_;
  std::string port_;
};

using NodeList = std::list<CircuitNode>;

class Circuit {
 public:
  using Json = nlohmann::json;

 public:
  Circuit();

 public:
  void Update(const Json& json);

  void Log() const;

  const NodeList& nodes() const { return nodes_; };

 private:
  NodeList nodes_;
};

}  // config
}  // ssf

#endif  // SSF_COMMON_CONFIG_CIRCUIT_H_
