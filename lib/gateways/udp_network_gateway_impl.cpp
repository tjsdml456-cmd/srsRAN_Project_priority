/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "udp_network_gateway_impl.h"
#include "srsran/adt/span.h"
#include "srsran/gateways/addr_info.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/io/sockets.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <optional>
// IP_RECVTOS may be defined in linux/in.h
#ifdef __linux__
#include <linux/in.h>
#endif

using namespace srsran;

udp_network_gateway_impl::udp_network_gateway_impl(udp_network_gateway_config                   config_,
                                                   network_gateway_data_notifier_with_src_addr& data_notifier_,
                                                   task_executor&                               io_tx_executor_,
                                                   task_executor&                               io_rx_executor_) :
  config(std::move(config_)),
  data_notifier(data_notifier_),
  logger(srslog::fetch_basic_logger("UDP-GW")),
  io_tx_executor(io_tx_executor_),
  io_rx_executor(io_rx_executor_),
  tx_ctx(config.tx_max_mmsg, config.tx_max_segments),
  batched_queue(
      config.tx_qsize,
      io_tx_executor,
      logger,
      [this](span<udp_tx_pdu_t> pdus) { handle_pdu_impl(pdus); },
      config.tx_max_mmsg)
{
  logger.info("UDP GW configured. rx_max_mmsg={} pool_thres={} ext_bind_addr={}",
              config.rx_max_mmsg,
              config.pool_occupancy_threshold,
              config.ext_bind_addr);
}

bool udp_network_gateway_impl::subscribe_to(io_broker& broker)
{
  io_subcriber = broker.register_fd(
      unique_fd(get_socket_fd()),
      io_rx_executor,
      [this]() { receive(); },
      [this](io_broker::error_code code) { handle_io_error(code); });
  if (not io_subcriber.registered()) {
    logger.error("Failed to register UDP network gateway at IO broker. socket_fd={}", get_socket_fd());
    return false;
  }
  logger.debug("Registered UDP network gateway at IO broker. socket_fd={}", get_socket_fd());
  return true;
}

void udp_network_gateway_impl::handle_pdu(byte_buffer pdu, const sockaddr_storage& dest_addr)
{
  if (not batched_queue.try_push(udp_tx_pdu_t{std::move(pdu), dest_addr})) {
    logger.info("Dropped PDU, queue is full.");
  }
}

void udp_network_gateway_impl::handle_pdu_impl(span<udp_tx_pdu_t> pdus)
{
  if (not sock_fd.is_open()) {
    logger.error("Socket not initialized");
    return;
  }

  unsigned msg_index     = 0;
  unsigned segment_index = 0;
  for (const auto& pdu : pdus) {
    if (pdu.pdu.length() > network_gateway_udp_max_len) {
      logger.error("PDU of {} bytes exceeds maximum length of {} bytes", pdu.pdu.length(), network_gateway_udp_max_len);
      break;
    }
    for (span segment : pdu.pdu.segments()) {
      const unsigned char* data                      = segment.begin();
      unsigned             size                      = segment.size();
      tx_ctx.msgs[msg_index][segment_index].iov_base = (void*)data;
      tx_ctx.msgs[msg_index][segment_index].iov_len  = size;
      segment_index++;
      if (segment_index >= config.tx_max_segments) {
        logger.error("Too many segments. Truncating PDU.");
        break;
      }
    }

    tx_ctx.mmsg[msg_index].msg_hdr.msg_iov        = tx_ctx.msgs[msg_index].data();
    tx_ctx.mmsg[msg_index].msg_hdr.msg_iovlen     = segment_index;
    tx_ctx.mmsg[msg_index].msg_hdr.msg_name       = (void*)&pdu.dst_addr;
    tx_ctx.mmsg[msg_index].msg_hdr.msg_namelen    = sizeof(pdu.dst_addr);
    tx_ctx.mmsg[msg_index].msg_hdr.msg_control    = nullptr;
    tx_ctx.mmsg[msg_index].msg_hdr.msg_controllen = 0;
    tx_ctx.mmsg[msg_index].msg_hdr.msg_flags      = 0;

    segment_index = 0;
    msg_index++;
    if (msg_index > 256) {
      logger.error("Too many SDUs to send in a single burst, dropping.");
      break;
    }
    if (logger.debug.enabled()) {
      std::string local_addr_str;
      std::string dest_addr_str;
      uint16_t    dest_port = sockaddr_to_port((sockaddr*)&pdu.dst_addr, logger);
      sockaddr_to_ip_str((sockaddr*)&pdu.dst_addr, dest_addr_str, logger);
      sockaddr_to_ip_str((sockaddr*)&local_addr, local_addr_str, logger);
      logger.debug(
          "Sent PDU of {} bytes. local_ip={} dest={}:{}", pdu.pdu.length(), local_addr_str, dest_addr_str, dest_port);
    }
  }

  int ret = ::sendmmsg(sock_fd.value(), tx_ctx.mmsg.data(), msg_index, 0);
  if (ret < 0) {
    logger.error("Could not send {} packets to socket. ret={} error={}", msg_index, ret, ::strerror(errno));
  }
}

void udp_network_gateway_impl::handle_io_error(io_broker::error_code code)
{
  logger.error("Error reading from UDP socket: {}", sock_fd.value());
}

bool udp_network_gateway_impl::create_and_bind()
{
  struct addrinfo hints;
  // support ipv4, ipv6 and hostnames
  hints.ai_family    = AF_UNSPEC;
  hints.ai_socktype  = SOCK_DGRAM;
  hints.ai_flags     = 0;
  hints.ai_protocol  = IPPROTO_UDP;
  hints.ai_canonname = nullptr;
  hints.ai_addr      = nullptr;
  hints.ai_next      = nullptr;

  std::string      bind_port = std::to_string(config.bind_port);
  struct addrinfo* results;

  int ret = ::getaddrinfo(config.bind_address.c_str(), bind_port.c_str(), &hints, &results);
  if (ret != 0) {
    logger.error("Getaddrinfo error: {} - {}", config.bind_address, ::gai_strerror(ret));
    return false;
  }

  struct addrinfo* result;
  for (result = results; result != nullptr; result = result->ai_next) {
    // create UDP socket
    sock_fd = unique_fd{::socket(result->ai_family, result->ai_socktype, result->ai_protocol), false};
    if (not sock_fd.is_open()) {
      ret = errno;
      continue;
    }

    char ip_addr[NI_MAXHOST];
    char port_nr[NI_MAXSERV];
    ::getnameinfo(
        result->ai_addr, result->ai_addrlen, ip_addr, NI_MAXHOST, port_nr, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
    logger.debug("Binding to {} port {}", ip_addr, port_nr);

    if (::bind(sock_fd.value(), result->ai_addr, result->ai_addrlen) == -1) {
      // binding failed, try next address
      ret = errno;
      logger.debug("Failed to bind to {}:{} - {}", ip_addr, port_nr, ::strerror(ret));
      close_socket();
      continue;
    }

    // Bind socket to interface (if requested)
    if (not bind_to_interface(sock_fd, config.bind_interface, logger)) {
      close_socket();
      continue;
    }

    // store client address
    std::memcpy(&local_addr, result->ai_addr, result->ai_addrlen);
    local_addrlen     = result->ai_addrlen;
    local_ai_family   = result->ai_family;
    local_ai_socktype = result->ai_socktype;
    local_ai_protocol = result->ai_protocol;

    // set socket to non-blocking after bind is successful
    if (config.non_blocking_mode) {
      if (not set_non_blocking()) {
        // failed, try next address
        logger.error("Socket not non-blocking");
        close_socket();
        continue;
      }
    }

    logger.debug("Binding successful");
    break;
  }

  ::freeaddrinfo(results);

  if (not sock_fd.is_open()) {
    fmt::print("Failed to bind {} socket to {}:{}. {}\n",
               ipproto_to_string(hints.ai_protocol),
               config.bind_address,
               config.bind_port,
               ::strerror(ret));
    logger.error("Failed to bind {} socket to {}:{}. {}",
                 ipproto_to_string(hints.ai_protocol),
                 config.bind_address,
                 config.bind_port,
                 ::strerror(ret));
    return false;
  }

  // Set socket options. This is done after binding,
  // so that we can set IPv4/IPv6 options accordingly.
  if (not set_sockopts()) {
    close_socket();
    return false;
  }

  return true;
}

namespace {

/// Receive context used by the recvmmsg syscall.
struct receive_context {
  explicit receive_context(unsigned rx_max_mmsg);

  std::vector<std::vector<uint8_t>> rx_mem;
  std::vector<::sockaddr_storage>   rx_srcaddr;
  std::vector<::mmsghdr>            rx_msghdr;
  std::vector<::iovec>              rx_iovecs;
  std::vector<std::vector<uint8_t>> rx_control; // Control messages for IP_PKTINFO
};

} // namespace

receive_context::receive_context(unsigned rx_max_mmsg)
{
  // Allocate RX buffers.
  rx_mem.resize(rx_max_mmsg);
  for (unsigned i = 0; i != rx_max_mmsg; ++i) {
    rx_mem[i].resize(network_gateway_udp_max_len);
  }

  // Allocate context for recv_mmsg.
  rx_srcaddr.resize(rx_max_mmsg);
  rx_msghdr.resize(rx_max_mmsg);
  rx_iovecs.resize(rx_max_mmsg);
  rx_control.resize(rx_max_mmsg);

  for (unsigned i = 0; i != rx_max_mmsg; ++i) {
    rx_msghdr[i].msg_hdr             = {};
    rx_msghdr[i].msg_hdr.msg_name    = &rx_srcaddr[i];
    rx_msghdr[i].msg_hdr.msg_namelen = sizeof(::sockaddr_storage);

    rx_iovecs[i].iov_base           = rx_mem[i].data();
    rx_iovecs[i].iov_len            = network_gateway_udp_max_len;
    rx_msghdr[i].msg_hdr.msg_iov    = &rx_iovecs[i];
    rx_msghdr[i].msg_hdr.msg_iovlen = 1;

    // Allocate control message buffer for IP_PKTINFO (ToS extraction)
    // Space for IP_PKTINFO + IP_TOS
    rx_control[i].resize(CMSG_SPACE(sizeof(struct in_pktinfo)) + CMSG_SPACE(sizeof(uint8_t)));
    rx_msghdr[i].msg_hdr.msg_control    = rx_control[i].data();
    rx_msghdr[i].msg_hdr.msg_controllen = rx_control[i].size();
  }
}

void udp_network_gateway_impl::receive()
{
  if (!sock_fd.is_open()) {
    logger.error("Cannot receive on UDP gateway: Socket is not initialized.");
  }

  thread_local receive_context rx_context(config.rx_max_mmsg);

  int rx_msgs = recvmmsg(sock_fd.value(), rx_context.rx_msghdr.data(), config.rx_max_mmsg, MSG_WAITFORONE, nullptr);
  srslog::fetch_basic_logger("IO-EPOLL").info("UDP rx {} packets, max is {}", rx_msgs, config.rx_max_mmsg);
  if (rx_msgs == -1 && errno != EAGAIN) {
    logger.error("Error reading from UDP socket: {}", ::strerror(errno));
    return;
  }
  if (rx_msgs == -1 && errno == EAGAIN) {
    if (!config.non_blocking_mode) {
      logger.debug("Socket timeout reached");
    }
    return;
  }

  // Log control message status for first packet only (to avoid spam)
  static bool first_packet_logged = false;
  if (!first_packet_logged && rx_msgs > 0) {
    struct msghdr* first_msg = &rx_context.rx_msghdr[0].msg_hdr;
    logger.warning("[IPTABLES-DSCP] First packet received: msg_controllen={} (after recvmmsg)", first_msg->msg_controllen);
    first_packet_logged = true;
  }

  for (int i = 0; i < rx_msgs; ++i) {
    float pool_occupancy =
        (1 - (float)get_byte_buffer_segment_pool_current_size_approx() / get_byte_buffer_segment_pool_capacity());
    if (pool_occupancy >= config.pool_occupancy_threshold) {
      if (warn_low_buffer_pool) {
        logger.warning("Buffer pool at {:.1f}% occupancy. Dropping {} packets", pool_occupancy * 100, rx_msgs - i);
        warn_low_buffer_pool = false;
        return;
      }
      logger.info("Buffer pool at {:.1f}% occupancy. Dropping {} packets", pool_occupancy * 100, rx_msgs - i);
      return;
    }
    span<uint8_t> payload(rx_context.rx_mem[i].data(), rx_context.rx_msghdr[i].msg_len);

    // Extract ToS from outer IP packet header (for iptables DSCP mapping)
    std::optional<uint8_t> outer_tos = {};
    struct msghdr* msg = &rx_context.rx_msghdr[i].msg_hdr;
    
    // Log every 100 packets to avoid spam
    static int packet_count = 0;
    bool should_log = (++packet_count % 100 == 0);
    
    // Check if control messages are available
    // Note: recvmmsg() updates msg_controllen to the actual size of received control messages
    if (msg->msg_controllen > 0) {
      if (should_log) {
        logger.info("[IPTABLES-DSCP] Control message available: msg_controllen={} (packet {})", msg->msg_controllen, packet_count);
      }
      
      bool found_tos = false;
      for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (should_log) {
          logger.info("[IPTABLES-DSCP] Control message: level={} type={} len={}", 
                       cmsg->cmsg_level, cmsg->cmsg_type, cmsg->cmsg_len);
        }
        
        if (cmsg->cmsg_level == IPPROTO_IP) {
          if (cmsg->cmsg_type == IP_TOS) {
            outer_tos = *(uint8_t*)CMSG_DATA(cmsg);
            found_tos = true;
            
            // Extract source IP and port for debugging (to identify UPF packets)
            char src_ip_str[INET6_ADDRSTRLEN] = {};
            uint16_t src_port = 0;
            if (msg->msg_name != nullptr) {
              sockaddr* src_addr = (sockaddr*)msg->msg_name;
              if (src_addr->sa_family == AF_INET) {
                sockaddr_in* src_in = (sockaddr_in*)src_addr;
                inet_ntop(AF_INET, &src_in->sin_addr, src_ip_str, INET6_ADDRSTRLEN);
                src_port = ntohs(src_in->sin_port);
              } else if (src_addr->sa_family == AF_INET6) {
                sockaddr_in6* src_in6 = (sockaddr_in6*)src_addr;
                inet_ntop(AF_INET6, &src_in6->sin6_addr, src_ip_str, INET6_ADDRSTRLEN);
                src_port = ntohs(src_in6->sin6_port);
              }
            }
            
            // Always log when ToS is extracted (important event) - include source info for debugging
            // Check if this is localhost traffic (127.0.0.x)
            bool is_localhost = (strncmp(src_ip_str, "127.0.0.", 8) == 0) || (strcmp(src_ip_str, "::1") == 0);
            const char* traffic_type = is_localhost ? "로컬호스트" : "외부";
            
            // Additional diagnostic: Check if iptables DSCP module is available
            // For localhost traffic, iptables INPUT chain may not work, and DSCP module may not be loaded
            const char* diagnostic_note = "";
            if (is_localhost && outer_tos.value() == 0x00) {
              diagnostic_note = " [진단: 로컬호스트+ToS=0x00 → UPF가 0x00으로 보냈거나 iptables DSCP 모듈 미로드(WSL2 제한 가능). 해결: UPF에서 ToS 설정 또는 실제 네트워크 인터페이스 사용]";
            } else if (is_localhost) {
              diagnostic_note = " [진단: 로컬호스트이지만 ToS가 0x00 아님 → iptables가 작동했거나 UPF가 ToS 설정]";
            } else if (outer_tos.value() == 0x00) {
              diagnostic_note = " [진단: 외부 트래픽+ToS=0x00 → UPF가 0x00으로 보냈거나 iptables 규칙 미적용]";
            }
            
            logger.warning("[IPTABLES-DSCP] Extracted outer IP ToS=0x{:02x} (DSCP={}) from UDP packet src={}:{} len={} type={}{}",
                        outer_tos.value(),
                        (outer_tos.value() >> 2) & 0x3F,
                        src_ip_str,
                        src_port,
                        rx_context.rx_msghdr[i].msg_len,
                        traffic_type,
                        diagnostic_note);
            break;
          } else if (cmsg->cmsg_type == IP_PKTINFO) {
            // Extract destination IP from IP_PKTINFO for debugging
            struct in_pktinfo* pktinfo = (struct in_pktinfo*)CMSG_DATA(cmsg);
            char dst_ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &pktinfo->ipi_addr, dst_ip_str, INET_ADDRSTRLEN);
            if (should_log) {
              logger.info("[IPTABLES-DSCP] Found IP_PKTINFO control message (type={}) dst_ip={} if_index={}", 
                          IP_PKTINFO, dst_ip_str, pktinfo->ipi_ifindex);
            }
          }
        }
      }
      
      if (!found_tos && should_log) {
        logger.warning("[IPTABLES-DSCP] Failed to extract outer IP ToS from UDP packet (IP_TOS control message not found, IP_RECVTOS may not be set)");
      }
    } else {
      // msg_controllen is 0, which means no control messages were received
      // Log this every 100 packets to avoid spam
      if (should_log) {
        logger.warning("[IPTABLES-DSCP] No control messages available (msg_controllen=0, packet {}) - IP_RECVTOS or IP_PKTINFO may not be working. Check socket options.", packet_count);
      }
    }

    byte_buffer pdu = {};
    if (pdu.append(payload)) {
      logger.debug("Received {} bytes on UDP socket. Pool occupancy {:.2f}%",
                   rx_context.rx_msghdr[i].msg_len,
                   pool_occupancy * 100);
      data_notifier.on_new_pdu(std::move(pdu), 
                               *(sockaddr_storage*)rx_context.rx_msghdr[i].msg_hdr.msg_name,
                               outer_tos);
    } else {
      logger.error("Could not allocate byte buffer. Received {} bytes on UDP socket", rx_context.rx_msghdr[i].msg_len);
    }
  }
}

int udp_network_gateway_impl::get_socket_fd()
{
  return sock_fd.value();
}

std::optional<uint16_t> udp_network_gateway_impl::get_bind_port() const
{
  if (not sock_fd.is_open()) {
    logger.error("Socket of UDP network gateway not initialized.");
    return {};
  }

  sockaddr_storage gw_addr_storage;
  sockaddr*        gw_addr     = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len = sizeof(gw_addr_storage);

  int ret = ::getsockname(sock_fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("Failed `getsockname` in UDP network gateway with sock_fd={}: {}", sock_fd.value(), ::strerror(errno));
    return {};
  }

  uint16_t gw_bind_port;
  if (gw_addr->sa_family == AF_INET) {
    gw_bind_port = ntohs(((sockaddr_in*)gw_addr)->sin_port);
  } else if (gw_addr->sa_family == AF_INET6) {
    gw_bind_port = ntohs(((sockaddr_in6*)gw_addr)->sin6_port);
  } else {
    logger.error("Unhandled address family in UDP network gateway with sock_fd={}, family={}",
                 sock_fd.value(),
                 gw_addr->sa_family);
    return {};
  }

  logger.debug("Read bind port of UDP network gateway: {}", gw_bind_port);
  return gw_bind_port;
}

bool udp_network_gateway_impl::get_bind_address(std::string& ip_address) const
{
  ip_address = "no address";

  if (not sock_fd.is_open()) {
    logger.error("Socket of UDP network gateway not initialized.");
    return false;
  }

  if (config.ext_bind_addr != "" and config.ext_bind_addr != "auto") {
    ip_address = config.ext_bind_addr;
    return true;
  }

  sockaddr_storage gw_addr_storage = {};
  sockaddr*        gw_addr         = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len     = sizeof(gw_addr_storage);

  int ret = ::getsockname(sock_fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("Failed `getsockname` in UDP network gateway with sock_fd={}: {}", sock_fd.value(), ::strerror(errno));
    return false;
  }

  char addr_str[INET6_ADDRSTRLEN] = {};
  if (gw_addr->sa_family == AF_INET) {
    if (inet_ntop(AF_INET, &((sockaddr_in*)gw_addr)->sin_addr, addr_str, INET6_ADDRSTRLEN) == nullptr) {
      logger.error("Could not convert sockaddr_in to string. sock_fd={}, errno={}", sock_fd.value(), ::strerror(errno));
      return false;
    }
  } else if (gw_addr->sa_family == AF_INET6) {
    if (inet_ntop(AF_INET6, &((sockaddr_in6*)gw_addr)->sin6_addr, addr_str, INET6_ADDRSTRLEN) == nullptr) {
      logger.error(
          "Could not convert sockaddr_in6 to string. sock_fd={}, errno={}", sock_fd.value(), ::strerror(errno));
      return false;
    }
  } else {
    logger.error("Unhandled address family in UDP network gateway with sock_fd={}", sock_fd.value());
    return false;
  }
  ip_address = addr_str;

  logger.debug("Read bind address of UDP network gateway: {}", ip_address);
  return true;
}

bool udp_network_gateway_impl::set_sockopts()
{
  if (config.rx_timeout_sec > 0) {
    if (not set_receive_timeout(config.rx_timeout_sec)) {
      logger.error("Couldn't set receive timeout for socket");

      return false;
    }
  }

  if (config.reuse_addr) {
    if (not set_reuse_addr()) {
      logger.error("Couldn't set reuseaddr for socket");
      return false;
    }
  }

  if (config.dscp.has_value()) {
    if (not set_dscp()) {
      logger.error("Couldn't set DSCP for socket");
      return false;
    }
  }

  // Enable IP_PKTINFO and IP_RECVTOS to receive ToS information from outer IP packet header
  // This allows iptables DSCP changes to be propagated to inner IP packets
  logger.warning("[IPTABLES-DSCP] Setting socket options: local_ai_family={}", local_ai_family);
  
  if (local_ai_family == AF_INET) {
    int val = 1;
    // IP_PKTINFO: Receive destination address information
    if (::setsockopt(sock_fd.value(), IPPROTO_IP, IP_PKTINFO, &val, sizeof(val)) < 0) {
      logger.warning("[IPTABLES-DSCP] Failed to set IP_PKTINFO socket option: {}. iptables DSCP mapping may not work.", ::strerror(errno));
      // Don't return false, as this is optional functionality
    } else {
      logger.warning("[IPTABLES-DSCP] Successfully set IP_PKTINFO socket option");
    }
    
    // IP_RECVTOS: Receive ToS information in control messages (required for IP_TOS control message)
    // IP_RECVTOS is defined in Linux 2.6.24+, value is typically 13
    #ifdef IP_RECVTOS
    int recvtos_opt = IP_RECVTOS;
    #else
    // IP_RECVTOS is typically 13 on Linux (defined in linux/in.h)
    // Try to use it even if not defined at compile time
    int recvtos_opt = 13; // IP_RECVTOS value on Linux
    logger.info("[IPTABLES-DSCP] IP_RECVTOS not defined at compile-time, using runtime value 13");
    #endif
    
    logger.info("[IPTABLES-DSCP] Attempting to set IP_RECVTOS socket option (value={})", recvtos_opt);
    if (::setsockopt(sock_fd.value(), IPPROTO_IP, recvtos_opt, &val, sizeof(val)) < 0) {
      logger.warning("[IPTABLES-DSCP] Failed to set IP_RECVTOS socket option (value={}): {}. iptables DSCP mapping may not work.", recvtos_opt, ::strerror(errno));
      // Don't return false, as this is optional functionality
    } else {
      logger.warning("[IPTABLES-DSCP] Successfully set IP_RECVTOS socket option (value={}) for ToS extraction", recvtos_opt);
    }
  } else if (local_ai_family == AF_INET6) {
    logger.info("[IPTABLES-DSCP] IPv6 detected, IP_RECVTOS not applicable (use IPV6_RECVTCLASS for IPv6)");
  } else {
    logger.warning("[IPTABLES-DSCP] Unknown address family: {}, IP_RECVTOS not set", local_ai_family);
  }

  logger.debug("Successfully set socket options");
  return true;
}

bool udp_network_gateway_impl::set_non_blocking()
{
  int flags = ::fcntl(sock_fd.value(), F_GETFL, 0);
  if (flags == -1) {
    logger.error("Error getting socket flags: {}", ::strerror(errno));
    return false;
  }

  int s = ::fcntl(sock_fd.value(), F_SETFL, flags | O_NONBLOCK);
  if (s == -1) {
    logger.error("Error setting socket to non-blocking mode: {}", ::strerror(errno));
    return false;
  }

  return true;
}

bool udp_network_gateway_impl::set_receive_timeout(unsigned rx_timeout_sec)
{
  struct timeval tv;
  tv.tv_sec  = rx_timeout_sec;
  tv.tv_usec = 0;

  if (::setsockopt(sock_fd.value(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv)) {
    logger.error("Couldn't set receive timeout for socket: {}", ::strerror(errno));
    return false;
  }

  return true;
}

bool udp_network_gateway_impl::set_reuse_addr()
{
  int one = 1;
  if (::setsockopt(sock_fd.value(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
    logger.error("Couldn't set reuseaddr for socket: {}", ::strerror(errno));
    return false;
  }
  return true;
}

bool udp_network_gateway_impl::set_dscp()
{
  if (not config.dscp.has_value()) {
    return true; // No DSCP code to set.
  }
  uint8_t option = config.dscp.value() << 2; // DSCP is the only the 6 most significant bits.
  if (local_addr.ss_family == AF_INET) {
    if (::setsockopt(sock_fd.value(), IPPROTO_IP, IP_TOS, &option, sizeof(option))) {
      logger.error("Couldn't set DSCP for socket: {}", ::strerror(errno));
      return false;
    }
    logger.debug("Set DSCP for socket. dscp={}", config.dscp.value());
    return true;
  }
  if (local_addr.ss_family == AF_INET6) {
    if (::setsockopt(sock_fd.value(), IPPROTO_IPV6, IPV6_TCLASS, &option, sizeof(option))) {
      logger.error("Couldn't set DSCP for socket: {}", ::strerror(errno));
      return false;
    }
    logger.debug("Set DSCP for socket. dscp={}", config.dscp.value());
    return true;
  }
  logger.error("Unknown socket familly when setting DSCP");
  return false;
}

bool udp_network_gateway_impl::close_socket()
{
  if (not sock_fd.close()) {
    logger.error("Error closing socket: {}", ::strerror(errno));
    return false;
  }
  return true;
}

