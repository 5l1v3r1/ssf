#ifndef SSF_SERVICES_ADMIN_ADMIN_H_
#define SSF_SERVICES_ADMIN_ADMIN_H_

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <ssf/network/manager.h>

#include "common/boost/fiber/basic_fiber_demux.hpp"
#include "common/boost/fiber/stream_fiber.hpp"
#include "common/utils/to_underlying.h"

#include "services/base_service.h"
#include "services/service_id.h"
#include "services/service_port.h"

#include "services/admin/admin_command.h"
#include "services/admin/command_factory.h"
#include "services/admin/requests/create_service_request.h"
#include "services/admin/requests/stop_service_request.h"

#include "core/factories/service_factory.h"
#include "core/factory_manager/service_factory_manager.h"

#include "services/user_services/base_user_service.h"

namespace ssf {
namespace services {
namespace admin {

template <typename Demux>
class Admin : public BaseService<Demux> {
 public:
  using BaseUserServicePtr =
      typename ssf::services::BaseUserService<Demux>::BaseUserServicePtr;
  using OnUserService =
      std::function<void(BaseUserServicePtr, const boost::system::error_code&)>;
  using OnInitialization =
      std::function<void(const boost::system::error_code&)>;

 private:
  using LocalPortType = typename Demux::local_port_type;
  using AdminPtr = std::shared_ptr<Admin>;
  using ServiceManager =
      ItemManager<typename ssf::BaseService<Demux>::BaseServicePtr>;

  using Parameters = typename ssf::BaseService<Demux>::Parameters;
  using FiberAcceptor = typename ssf::BaseService<Demux>::fiber_acceptor;
  using Fiber = typename ssf::BaseService<Demux>::fiber;
  using FiberEndpoint = typename ssf::BaseService<Demux>::endpoint;

  using CommandHandler = std::function<void(const boost::system::error_code&)>;
  using IdToCommandHandlerMap = std::map<uint32_t, CommandHandler>;

 public:
  static AdminPtr Create(boost::asio::io_service& io_service,
                         Demux& fiber_demux, const Parameters& parameters) {
    return AdminPtr(new Admin(io_service, fiber_demux));
  }

  ~Admin() { SSF_LOG("microservice", trace, "[admin] destroy"); }

  enum {
    kFactoryId = to_underlying(MicroserviceId::kAdmin),
    kServicePort = to_underlying(MicroservicePort::kAdmin),
    kKeepAliveInterval = 120,      // seconds
    kServiceStatusRetryCount = 50  // retries
  };

  static void RegisterToServiceFactory(
      std::shared_ptr<ServiceFactory<Demux>> p_factory) {
    auto creator = [](boost::asio::io_service& io_service, Demux& fiber_demux,
                      const Parameters& parameters) {
      return Admin::Create(io_service, fiber_demux, parameters);
    };
    p_factory->RegisterServiceCreator(kFactoryId, creator);
  }

  template <template <class> class Command>
  bool RegisterCommand() {
    return cmd_factory_.template Register<Command<Demux>>();
  }

  void SetAsServer();
  void SetAsClient(std::vector<BaseUserServicePtr> user_services,
                   OnUserService on_user_service,
                   OnInitialization on_initialization);

  template <typename Request, typename Handler>
  void Command(Request request, Handler handler) {
    std::string parameters_buff_to_send = request.OnSending();

    auto serial = GetAvailableSerial();
    InsertHandler(serial, handler);

    auto p_command = std::make_shared<AdminCommand>(
        serial, request.command_id, (uint32_t)parameters_buff_to_send.size(),
        parameters_buff_to_send);

    auto do_handler = [p_command](const boost::system::error_code& ec,
                                  size_t length) {};

    AsyncSendCommand(*p_command, do_handler);
  }

  void InsertHandler(uint32_t serial, CommandHandler command_handler) {
    std::unique_lock<std::recursive_mutex> lock1(command_handlers_mutex_);
    command_handlers_[serial] = command_handler;
  }

  // execute handler bound to the command serial id if exists
  void ExecuteAndRemoveCommandHandler(uint32_t serial) {
    if (command_handlers_.count(command_serial_received_)) {
      auto self = this->shared_from_this();
      auto command_handler = command_handlers_[command_serial_received_];
      this->get_io_service().post(
          [self, command_handler]() { command_handler({}); });
      this->EraseHandler(command_serial_received_);
    }
  }

  void EraseHandler(uint32_t serial) {
    std::unique_lock<std::recursive_mutex> lock1(command_handlers_mutex_);
    command_handlers_.erase(serial);
  }

  uint32_t GetAvailableSerial() {
    std::unique_lock<std::recursive_mutex> lock1(command_handlers_mutex_);

    for (uint32_t serial = 3; serial < std::numeric_limits<uint32_t>::max();
         ++serial) {
      if (command_handlers_.count(serial + !!is_server_) == 0) {
        command_handlers_[serial + !!is_server_] =
            [](const boost::system::error_code&) {};
        return serial + !!is_server_;
      }
    }
    return 0;
  }

 public:
  // BaseService
  void start(boost::system::error_code& ec);
  void stop(boost::system::error_code& ec);
  uint32_t service_type_id();

 private:
  Admin(boost::asio::io_service& io_service, Demux& fiber_demux);

  void AsyncAccept();
  void OnFiberAccept(const boost::system::error_code& ec);

  void AsyncConnect();
  void OnFiberConnect(const boost::system::error_code& ec);

  void HandleStop();

  void Initialize();
  void StartRemoteService(
      const admin::CreateServiceRequest<Demux>& create_request,
      const CommandHandler& handler);
  void StopRemoteService(const admin::StopServiceRequest<Demux>& stop_request,
                         const CommandHandler& handler);
  void InitializeRemoteServices(const boost::system::error_code& ec);
  void ListenForCommand();
  void DoAdmin(
      const boost::system::error_code& ec = boost::system::error_code(),
      size_t length = 0);
  void PostKeepAlive(const boost::system::error_code& ec, size_t length);
  void OnSendKeepAlive(const boost::system::error_code& ec);
  void ReceiveInstructionHeader();
  void ReceiveInstructionParameters();
  void ProcessInstructionId();

  template <typename Handler>
  void AsyncSendCommand(const AdminCommand& command, Handler handler) {
    auto do_handler = [handler](const boost::system::error_code& ec,
                                size_t length) { handler(ec, length); };

    boost::asio::async_write(fiber_, command.const_buffers(), do_handler);
  }

  void NotifyUserService(BaseUserServicePtr p_user_service,
                         const boost::system::error_code& ec) {
    if (is_server_) {
      return;
    }

    auto self = this->shared_from_this();
    this->get_io_service().post([this, self, p_user_service, ec]() {
      on_user_service_(p_user_service, ec);
    });
  }

  void NotifyInitialization(const boost::system::error_code& ec) {
    if (is_server_) {
      return;
    }

    auto self = this->shared_from_this();
    this->get_io_service().post([this, self, ec]() { on_initialization_(ec); });
  }

 private:
  // Version information
  uint8_t admin_version_;

  // For initiating fiber connection
  bool is_server_;

  FiberAcceptor fiber_acceptor_;
  Fiber fiber_;

  // The status in which the admin service is
  uint32_t status_;

  // Buffers to receive commands
  uint32_t command_serial_received_;
  uint32_t command_id_received_;
  uint32_t command_size_received_;
  std::vector<char> parameters_buff_received_;

  // Keep alives
  uint32_t reserved_keep_alive_id_;
  uint32_t reserved_keep_alive_size_;
  std::string reserved_keep_alive_parameters_;
  boost::asio::steady_timer reserved_keep_alive_timer_;

  // List of user services
  std::vector<BaseUserServicePtr> user_services_;

  // Connection attempts
  uint8_t retries_;

  // Is stopped?
  std::recursive_mutex stopping_mutex_;
  bool stopped_;

  // Initialize services
  boost::asio::coroutine coroutine_;
  size_t i_;
  std::vector<admin::CreateServiceRequest<Demux>> create_request_vector_;
  size_t j_;
  uint32_t remote_all_started_;
  uint16_t init_retries_;
  std::vector<admin::StopServiceRequest<Demux>> stop_request_vector_;
  bool local_all_started_;
  boost::system::error_code init_ec_;

  std::recursive_mutex command_handlers_mutex_;
  IdToCommandHandlerMap command_handlers_;

  OnUserService on_user_service_;
  OnInitialization on_initialization_;

  CommandFactory<Demux> cmd_factory_;
};

}  // admin
}  // services
}  // ssf

#include "services/admin/admin.ipp"

#endif  // SSF_SERVICES_ADMIN_ADMIN_H_
