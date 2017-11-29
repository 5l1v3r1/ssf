#ifndef SSF_LAYER_BASIC_PROXY_PROTOCOL_H_
#define SSF_LAYER_BASIC_PROXY_PROTOCOL_H_

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/io_service.hpp>

#include "ssf/layer/basic_endpoint.h"
#include "ssf/layer/basic_impl.h"
#include "ssf/layer/basic_resolver.h"

#include "ssf/layer/parameters.h"
#include "ssf/layer/protocol_attributes.h"

#include "ssf/layer/proxy/proxy_endpoint_context.h"
#include "ssf/layer/proxy/basic_proxy_acceptor_service.h"
#include "ssf/layer/proxy/basic_proxy_socket_service.h"

#include "ssf/layer/proxy/proxy_helpers.h"

namespace ssf {
namespace layer {
namespace proxy {

template <class NextLayer>
class basic_ProxyProtocol {
 public:
  enum {
    id = 6,
    overhead = 0,
    facilities = ssf::layer::facilities::stream,
    mtu = NextLayer::mtu - overhead
  };
  enum { endpoint_stack_size = 1 + NextLayer::endpoint_stack_size };

  static const char* NAME;

  typedef NextLayer next_layer_protocol;
  typedef int socket_context;
  typedef int acceptor_context;
  typedef ProxyEndpointContext endpoint_context_type;
  using next_endpoint_type = typename next_layer_protocol::endpoint;

  typedef basic_VirtualLink_endpoint<basic_ProxyProtocol> endpoint;
  typedef basic_VirtualLink_resolver<basic_ProxyProtocol> resolver;
  typedef boost::asio::basic_stream_socket<
      basic_ProxyProtocol, basic_ProxySocket_service<basic_ProxyProtocol>>
      socket;
  typedef boost::asio::basic_socket_acceptor<
      basic_ProxyProtocol, basic_ProxyAcceptor_service<basic_ProxyProtocol>>
      acceptor;

 private:
  using query = typename resolver::query;

  static std::string get_name() {
    std::string name(NAME);
    name += "_" + next_layer_protocol::get_name();
    return name;
  }

 public:
  static endpoint make_endpoint(boost::asio::io_service& io_service,
                                typename query::const_iterator parameters_it,
                                uint32_t, boost::system::error_code& ec) {
    auto context = MakeProxyContext(io_service, *parameters_it, ec);

    if (ec) {
      return endpoint();
    }

    // Get next layer parameters
    ++parameters_it;

    if (!context.acceptor_endpoint() &&
        context.UpdateRemoteHost(*parameters_it)) {
      // Proxy context contains remote host data
      return endpoint(context);
    } else {
      return endpoint(context, next_layer_protocol::make_endpoint(
                                   io_service, parameters_it, id, ec));
    }
  }
};

template <class NextLayer>
const char* basic_ProxyProtocol<NextLayer>::NAME = "PROXY";

}  // proxy
}  // layer
}  // layer

#endif  // SSF_LAYER_BASIC_PROXY_PROTOCOL_H_
