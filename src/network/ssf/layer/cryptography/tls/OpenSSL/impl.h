#ifndef SSF_LAYER_CRYPTOGRAPHY_TLS_OPENSSL_IMPL_H_
#define SSF_LAYER_CRYPTOGRAPHY_TLS_OPENSSL_IMPL_H_

#include <cstdint>

#include <functional>
#include <memory>
#include <mutex>

#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/handler_type_requirements.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

#include <boost/system/error_code.hpp>

#include "ssf/layer/cryptography/tls/OpenSSL/helpers.h"

#include "ssf/error/error.h"
#include "ssf/io/read_stream_op.h"

#include "ssf/layer/basic_endpoint.h"
#include "ssf/layer/parameters.h"
#include "ssf/layer/protocol_attributes.h"

#include "ssf/log/log.h"

namespace ssf {
namespace layer {
namespace cryptography {

namespace detail {

/// The class in charge of receiving data from a TLS stream into a buffer
template <typename NextLayerStreamSocket>
class TLSStreamBufferer : public std::enable_shared_from_this<
                              TLSStreamBufferer<NextLayerStreamSocket>> {
 private:
  enum {
    lower_queue_size_bound = 1 * 1024 * 1024,
    higher_queue_size_bound = 16 * 1024 * 1024,
    receive_buffer_size = 50 * 1024
  };

 private:
  typedef boost::asio::ssl::stream<NextLayerStreamSocket> tls_stream_type;
  typedef std::shared_ptr<tls_stream_type> p_tls_stream_type;
  typedef detail::ExtendedTLSContext p_context_type;
  typedef boost::asio::io_service::strand strand_type;
  typedef std::shared_ptr<strand_type> p_strand_type;

 public:
  typedef TLSStreamBufferer<NextLayerStreamSocket> puller_type;
  typedef std::shared_ptr<puller_type> p_puller_type;
  typedef boost::asio::detail::op_queue<io::basic_pending_read_stream_operation>
      op_queue_type;

 public:
  TLSStreamBufferer(const TLSStreamBufferer&) = delete;
  TLSStreamBufferer& operator=(const TLSStreamBufferer&) = delete;

  ~TLSStreamBufferer() {}

  static p_puller_type create(p_tls_stream_type p_socket,
                              p_strand_type p_strand) {
    return p_puller_type(new puller_type(p_socket, p_strand));
  }

  /// Start receiving data
  void start_pulling() {
    std::unique_lock<std::recursive_mutex> lock(pulling_mutex_);
    if (!pulling_) {
      pulling_ = true;
      SSF_LOG("network_crypto", debug, "pulling");
      io_service_.post(std::bind(&TLSStreamBufferer::async_pull_packets,
                                 this->shared_from_this()));
    }
  }

  /// User interface for receiving some data
  template <typename MutableBufferSequence, typename ReadHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
                                void(boost::system::error_code, std::size_t))
  async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler) {
    boost::asio::detail::async_result_init<
        ReadHandler, void(boost::system::error_code, std::size_t)>
        init(std::forward<ReadHandler>(handler));

    auto buffer_size = boost::asio::buffer_size(buffers);

    if (buffer_size) {
      typedef io::pending_read_stream_operation<MutableBufferSequence,
                                                decltype(init.handler)>
          op;
      typename op::ptr p = {
          boost::asio::detail::addressof(init.handler),
          boost_asio_handler_alloc_helpers::allocate(sizeof(op), init.handler),
          0};

      p.p = new (p.v) op(buffers, init.handler);

      {
        std::unique_lock<std::recursive_mutex> lock(op_queue_mutex_);
        op_queue_.push(p.p);
      }

      p.v = p.p = 0;

      io_service_.post(std::bind(&TLSStreamBufferer::handle_data_n_ops,
                                 this->shared_from_this()));
    } else {
      io_service_.post(std::bind(init.handler, boost::system::error_code(), 0));
    }

    return init.result.get();
  }

  boost::system::error_code cancel(boost::system::error_code& ec) {
    {
      std::unique_lock<std::recursive_mutex> lock(pulling_mutex_);
      pulling_ = false;
    }

    std::unique_lock<std::recursive_mutex> lock1(data_queue_mutex_);
    std::unique_lock<std::recursive_mutex> lock2(op_queue_mutex_);
    data_queue_.consume(data_queue_.size());
    while (!op_queue_.empty()) {
      auto op = op_queue_.front();
      op_queue_.pop();
      auto self = this->shared_from_this();
      auto do_complete = [self, op]() {
        op->complete(boost::asio::error::make_error_code(
                         boost::asio::error::basic_errors::operation_aborted),
                     0);
      };
      io_service_.post(do_complete);
    }

    return ec;
  }

 private:
  TLSStreamBufferer(p_tls_stream_type p_socket, p_strand_type p_strand)
      : socket_(*p_socket),
        p_socket_(p_socket),
        strand_(*p_strand),
        p_strand_(p_strand),
        io_service_(strand_.get_io_service()),
        status_(boost::system::error_code()),
        pulling_(false) {}

  /// Check if data is available for user requests
  void handle_data_n_ops() {
    auto self = this->shared_from_this();

    if (!status_) {
      {
        std::unique_lock<std::recursive_mutex> lock(pulling_mutex_);
        if ((data_queue_.size() < lower_queue_size_bound) && !pulling_) {
          start_pulling();
        }
      }

      std::unique_lock<std::recursive_mutex> lock1(data_queue_mutex_);
      std::unique_lock<std::recursive_mutex> lock2(op_queue_mutex_);
      if (!op_queue_.empty() && data_queue_.size()) {
        auto op = op_queue_.front();
        op_queue_.pop();

        size_t copied = op->fill_buffer(data_queue_);

        auto do_complete = [self, op, copied]() {
          op->complete(boost::system::error_code(), copied);
        };
        io_service_.post(do_complete);

        io_service_.dispatch(std::bind(&TLSStreamBufferer::handle_data_n_ops,
                                       this->shared_from_this()));
      }
    } else {
      std::unique_lock<std::recursive_mutex> lock1(data_queue_mutex_);
      std::unique_lock<std::recursive_mutex> lock2(op_queue_mutex_);
      if (!op_queue_.empty()) {
        auto op = op_queue_.front();
        op_queue_.pop();
        auto do_complete = [this, self, op]() { op->complete(status_, 0); };
        io_service_.post(do_complete);

        io_service_.dispatch(std::bind(&TLSStreamBufferer::handle_data_n_ops,
                                       this->shared_from_this()));
      }
    }
  }

  /// Receive some data
  void async_pull_packets() {
    auto self = this->shared_from_this();

    auto handler = [this, self](const boost::system::error_code& ec,
                                size_t length) {
      if (!ec) {
        {
          std::unique_lock<std::recursive_mutex> lock1(this->data_queue_mutex_);
          this->data_queue_.commit(length);
        }

        if (!this->status_) {
          this->io_service_.dispatch(
              std::bind(&TLSStreamBufferer::async_pull_packets,
                        this->shared_from_this()));
        }
      } else {
        if (ec.value() == boost::asio::error::operation_aborted) {
          boost::system::error_code cancel_ec;
          cancel(cancel_ec);
        } else {
          data_queue_.consume(data_queue_.size());
          this->status_ = ec;
          SSF_LOG("network_crypto", debug, "TLS connection terminated ({}: {})",
                  ec.value(), ec.message());
        }
      }

      this->io_service_.dispatch(std::bind(
          &TLSStreamBufferer::handle_data_n_ops, this->shared_from_this()));
    };

    {
      std::unique_lock<std::recursive_mutex> lock(data_queue_mutex_);
      boost::asio::streambuf::mutable_buffers_type bufs =
          data_queue_.prepare(receive_buffer_size);

      if (data_queue_.size() < higher_queue_size_bound) {
        auto async_read_some = [this, self, bufs, handler]() {
          socket_.async_read_some(bufs, strand_.wrap(handler));
        };
        strand_.dispatch(async_read_some);
      } else {
        pulling_ = false;
        SSF_LOG("network_crypto", debug, "not pulling");
      }
    }
  }

  /// The TLS stream to receive from
  tls_stream_type& socket_;
  p_tls_stream_type p_socket_;

  /// The strand to insure that read and write are not concurrent
  strand_type& strand_;
  p_strand_type p_strand_;

  /// The io_service handling asynchronous operations
  boost::asio::io_service& io_service_;

  /// Errors during async_read are saved here
  boost::system::error_code status_;

  /// Handle the data received
  std::recursive_mutex data_queue_mutex_;
  boost::asio::streambuf data_queue_;

  /// Handle pending user operations
  std::recursive_mutex op_queue_mutex_;
  op_queue_type op_queue_;

  std::recursive_mutex pulling_mutex_;
  bool pulling_;
};

}  // detail

/// The class wrapping a TLS stream to allow buffer optimization, non
/// concurrent io and move operations
template <typename NextLayerStreamSocket>
class basic_buffered_tls_socket {
 private:
  typedef boost::asio::ssl::stream<NextLayerStreamSocket> tls_stream_type;
  typedef std::shared_ptr<tls_stream_type> p_tls_stream_type;
  typedef detail::ExtendedTLSContext p_context_type;
  typedef std::shared_ptr<boost::asio::streambuf> p_streambuf;
  typedef detail::TLSStreamBufferer<NextLayerStreamSocket> puller_type;
  typedef std::shared_ptr<puller_type> p_puller_type;
  typedef boost::asio::io_service::strand strand_type;
  typedef std::shared_ptr<strand_type> p_strand_type;

 public:
  typedef typename tls_stream_type::next_layer_type next_layer_type;
  typedef typename tls_stream_type::lowest_layer_type lowest_layer_type;
  typedef typename tls_stream_type::handshake_type handshake_type;

 public:
  basic_buffered_tls_socket()
      : p_ctx_(nullptr),
        p_socket_(nullptr),
        socket_(),
        p_strand_(nullptr),
        p_puller_(nullptr) {}

  basic_buffered_tls_socket(p_tls_stream_type p_socket, p_context_type p_ctx)
      : p_ctx_(p_ctx),
        p_socket_(p_socket),
        socket_(*p_socket_),
        p_strand_(std::make_shared<strand_type>(
            socket_.get().lowest_layer().get_io_service())),
        p_puller_(puller_type::create(p_socket_, p_strand_)) {}

  basic_buffered_tls_socket(boost::asio::io_service& io_service,
                            p_context_type p_ctx)
      : p_ctx_(p_ctx),
        p_socket_(new tls_stream_type(io_service, *p_ctx)),
        socket_(*p_socket_),
        p_strand_(std::make_shared<strand_type>(io_service)),
        p_puller_(puller_type::create(p_socket_, p_strand_)) {}

  basic_buffered_tls_socket(basic_buffered_tls_socket&& other)
      : p_ctx_(std::move(other.p_ctx_)),
        p_socket_(std::move(other.p_socket_)),
        socket_(*p_socket_),
        p_strand_(std::move(other.p_strand_)),
        p_puller_(std::move(other.p_puller_)) {
    other.socket_ = *(other.p_socket_);
  }

  ~basic_buffered_tls_socket() {}

  basic_buffered_tls_socket(const basic_buffered_tls_socket&) = delete;
  basic_buffered_tls_socket& operator=(const basic_buffered_tls_socket&) =
      delete;

  boost::asio::io_service& get_io_service() {
    return socket_.get().lowest_layer().get_io_service();
  }

  lowest_layer_type& lowest_layer() { return socket_.get().lowest_layer(); }
  next_layer_type& next_layer() { return socket_.get().next_layer(); }

  boost::system::error_code handshake(handshake_type type,
                                      boost::system::error_code& ec) {
    socket_.get().handshake(type, ec);

    if (!ec) {
      p_puller_->start_pulling();
    }

    return ec;
  }

  /// Forward the call to the TLS stream and start pulling packets on
  /// completion
  template <typename Handler>
  void async_handshake(handshake_type type, Handler handler) {
    auto p_puller = p_puller_;
    auto p_socket = p_socket_;
    auto p_strand = p_strand_;
    auto do_user_handler =
        [this, p_puller, handler](const boost::system::error_code& ec) mutable {
          if (!ec) {
            p_puller->start_pulling();
          } else {
            SSF_LOG("network_crypto", debug, "TLS handshake failed");
          }
          handler(ec);
        };

    auto async_handshake = [p_socket, p_strand, type, do_user_handler]() {
      p_socket->async_handshake(type, p_strand->wrap(do_user_handler));
    };
    p_strand_->dispatch(async_handshake);
  }

  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers,
                        boost::system::error_code& ec) {
    try {
      auto read = async_read_some(buffers, boost::asio::use_future);
      ec.assign(ssf::error::success, ssf::error::get_ssf_category());
      return read.get();
    } catch (const std::exception&) {
      ec.assign(ssf::error::io_error, ssf::error::get_ssf_category());
      return 0;
    }
  }

  /// Forward the call to the TLSStreamBufferer object
  template <typename MutableBufferSequence, typename ReadHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
                                void(boost::system::error_code, std::size_t))
  async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler) {
    return p_puller_->async_read_some(buffers,
                                      std::forward<ReadHandler>(handler));
  }

  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers,
                         boost::system::error_code& ec) {
    return socket_.get().write_some(buffers, ec);
  }

  /// Forward the call directly to the TLS stream (wrapped in an strand)
  template <typename ConstBufferSequence, typename WriteHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
                                void(boost::system::error_code, std::size_t))
  async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler) {
    boost::asio::detail::async_result_init<
        WriteHandler, void(boost::system::error_code, std::size_t)>
        init(std::forward<WriteHandler>(handler));
    auto p_socket = p_socket_;
    auto async_write_some = [this, p_socket, buffers, init]() {
      socket_.get().async_write_some(buffers, p_strand_->wrap(init.handler));
    };
    p_strand_->dispatch(async_write_some);

    return init.result.get();
  }

  /// Forward the call to the lowest layer of the TLS stream
  bool is_open() { return socket_.get().lowest_layer().is_open(); }

  void close() {
    boost::system::error_code ec;
    close(ec);
  }

  boost::system::error_code close(boost::system::error_code& ec) {
    auto result = socket_.get().lowest_layer().close(ec);
    if (p_puller_) {
      boost::system::error_code cancel_ec;
      p_puller_->cancel(cancel_ec);
    }
    return result;
  }

  void shutdown(boost::asio::socket_base::shutdown_type type,
                boost::system::error_code& ec) {
    socket_.get().lowest_layer().shutdown(type, ec);
  }

  boost::asio::ssl::context& context() { return *p_ctx_; }
  tls_stream_type& socket() { return socket_; }
  strand_type& strand() { return *p_strand_; }

 private:
  /// The TLS ctx in a shared_ptr to be able to move it
  p_context_type p_ctx_;

  /// The TLS stream in a shared_ptr to be able to move it
  p_tls_stream_type p_socket_;

  /// A reference to the TLS stream to avoid dereferencing a pointer
  std::reference_wrapper<tls_stream_type> socket_;

  /// The strand in a shared_ptr to be able to move it
  p_strand_type p_strand_;

  /// The TLSStreamBufferer in a shared_ptr to be able to move it
  p_puller_type p_puller_;
};

template <typename NextLayerStreamSocket>
class basic_tls_socket {
 private:
  typedef boost::asio::ssl::stream<NextLayerStreamSocket> tls_stream_type;
  typedef std::shared_ptr<tls_stream_type> p_tls_stream_type;
  typedef detail::ExtendedTLSContext p_context_type;
  typedef std::shared_ptr<boost::asio::streambuf> p_streambuf;
  typedef boost::asio::io_service::strand strand_type;
  typedef std::shared_ptr<strand_type> p_strand_type;

 public:
  typedef typename tls_stream_type::next_layer_type next_layer_type;
  typedef typename tls_stream_type::lowest_layer_type lowest_layer_type;
  typedef typename tls_stream_type::handshake_type handshake_type;

 public:
  basic_tls_socket()
      : p_ctx_(nullptr), p_socket_(nullptr), socket_(), p_strand_(nullptr) {}

  basic_tls_socket(p_tls_stream_type p_socket, p_context_type p_ctx)
      : p_ctx_(p_ctx),
        p_socket_(p_socket),
        socket_(*p_socket_),
        p_strand_(std::make_shared<strand_type>(
            socket_.get().lowest_layer().get_io_service())) {}

  basic_tls_socket(boost::asio::io_service& io_service, p_context_type p_ctx)
      : p_ctx_(p_ctx),
        p_socket_(new tls_stream_type(io_service, *p_ctx)),
        socket_(*p_socket_),
        p_strand_(std::make_shared<strand_type>(io_service)) {}

  basic_tls_socket(basic_tls_socket&& other)
      : p_ctx_(std::move(other.p_ctx_)),
        p_socket_(std::move(other.p_socket_)),
        socket_(*p_socket_),
        p_strand_(std::move(other.p_strand_)) {
    other.socket_ = *(other.p_socket_);
  }

  ~basic_tls_socket() {}

  basic_tls_socket(const basic_tls_socket&) = delete;
  basic_tls_socket& operator=(const basic_tls_socket&) = delete;

  boost::asio::io_service& get_io_service() {
    return socket_.get().lowest_layer().get_io_service();
  }

  lowest_layer_type& lowest_layer() { return socket_.get().lowest_layer(); }
  next_layer_type& next_layer() { return socket_.get().next_layer(); }

  boost::system::error_code handshake(handshake_type type,
                                      boost::system::error_code ec) {
    socket_.get().handshake(type, ec);
    return ec;
  }

  /// Forward the call to the TLS stream and start pulling packets on
  /// completion
  template <typename Handler>
  void async_handshake(handshake_type type, Handler handler) {
    auto lambda = [this, type, handler]() {
      socket_.get().async_handshake(type, p_strand_->wrap(handler));
    };

    p_strand_->dispatch(lambda);
  }

  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers,
                        boost::system::error_code& ec) {
    return socket_.get().read_some(buffers, ec);
  }

  /// Forward the call to the TLSStreamBufferer object
  template <typename MutableBufferSequence, typename ReadHandler>
  void async_read_some(const MutableBufferSequence& buffers,
                       ReadHandler&& handler) {
    auto lambda = [this, buffers, handler]() {
      socket_.get().async_read_some(buffers, p_strand_->wrap(handler));
    };
    p_strand_->dispatch(lambda);
  }

  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers,
                         boost::system::error_code& ec) {
    return socket_.get().write_some(buffers, ec);
  }

  /// Forward the call directly to the TLS stream (wrapped in an strand)
  template <typename ConstBufferSequence, typename Handler>
  void async_write_some(const ConstBufferSequence& buffers, Handler&& handler) {
    auto lambda = [this, buffers, handler]() {
      socket_.get().async_write_some(buffers, p_strand_->wrap(handler));
    };
    p_strand_->dispatch(lambda);
  }

  /// Forward the call to the lowest layer of the TLS stream
  bool is_open() { return socket_.get().lowest_layer().is_open(); }

  void close() {
    boost::system::error_code ec;
    socket_.get().lowest_layer().close(ec);
  }

  boost::system::error_code close(boost::system::error_code& ec) {
    return socket_.get().lowest_layer().close(ec);
  }

  void shutdown(boost::asio::socket_base::shutdown_type type,
                boost::system::error_code& ec) {
    socket_.get().lowest_layer().shutdown(type, ec);
  }

  boost::asio::ssl::context& context() { return *p_ctx_; }
  tls_stream_type& socket() { return socket_; }
  strand_type& strand() { return *p_strand_; }

 private:
  /// The TLS ctx in a shared_ptr to be able to move it
  p_context_type p_ctx_;

  /// The TLS stream in a shared_ptr to be able to move it
  p_tls_stream_type p_socket_;

  /// A reference to the TLS stream to avoid dereferencing a pointer
  std::reference_wrapper<tls_stream_type> socket_;

  /// The strand in a shared_ptr to be able to move it
  p_strand_type p_strand_;
};

template <class NextLayer, template <class> class TLSStreamSocket>
class basic_tls {
 public:
  enum {
    id = 2,
    overhead = 0,
    facilities = ssf::layer::facilities::stream,
    mtu = NextLayer::mtu - overhead
  };

  static const char* NAME;

  enum { endpoint_stack_size = 1 + NextLayer::endpoint_stack_size };

  using handshake_type = boost::asio::ssl::stream_base::handshake_type;

  using endpoint_context_type = detail::ExtendedTLSContext;
  using Stream = TLSStreamSocket<typename NextLayer::socket>;

 private:
  using query = ParameterStack;

 public:
  static std::string get_name() { return NAME; }

  static endpoint_context_type make_endpoint_context(
      boost::asio::io_service& io_service,
      typename query::const_iterator parameters_it, uint32_t lower_id,
      boost::system::error_code& ec) {
    auto context = detail::make_tls_context(io_service, *parameters_it);
    if (!context) {
      SSF_LOG("network_crypto", error, "could not generate context");
      ec.assign(ssf::error::invalid_argument, ssf::error::get_ssf_category());
    }

    return context;
  }
};

template <class NextLayer, template <class> class TLSStreamSocket>
const char* basic_tls<NextLayer, TLSStreamSocket>::NAME = "TLS";

template <class NextLayer>
using buffered_tls = basic_tls<NextLayer, basic_buffered_tls_socket>;

template <class NextLayer>
using tls = basic_tls<NextLayer, basic_tls_socket>;

}  // cryptography
}  // layer
}  // ssf

#endif  // SSF_LAYER_CRYPTOGRAPHY_TLS_OPENSSL_IMPL_H_
