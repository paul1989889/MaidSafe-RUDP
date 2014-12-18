/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/rudp/managed_connections.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>

#include "maidsafe/common/error.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/make_unique.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/rudp/connection.h"
#include "maidsafe/rudp/contact.h"
#include "maidsafe/rudp/nat_type.h"
#include "maidsafe/rudp/transport.h"
#include "maidsafe/rudp/utils.h"

namespace args = std::placeholders;
namespace bptime = boost::posix_time;

namespace maidsafe {

namespace rudp {

#ifdef TESTING
void SetDebugPacketLossRate(double constant, double bursty) {
  detail::Multiplexer::SetDebugPacketLossRate(constant, bursty);
}
#endif

namespace {

// Legacy functors to be removed
using MessageReceivedFunctor = std::function<void(const std::string& /*message*/)>;
using ConnectionLostFunctor = std::function<void(const NodeId& /*peer_id*/)>;

}  // unnamed namespace

ManagedConnections::PendingConnection::PendingConnection(NodeId node_id_in, TransportPtr transport,
                                                         boost::asio::io_service& io_service)
    : node_id(std::move(node_id_in)),
      pending_transport(std::move(transport)),
      timer(io_service,
            bptime::microsec_clock::universal_time() + Parameters::rendezvous_connect_timeout),
      connecting(false) {}

ManagedConnections::ManagedConnections()
    : asio_service_(Parameters::thread_count),
      listener_(),
      this_node_id_(),
      chosen_bootstrap_contact_(),
      keys_(),
      connections_(),
      pendings_(),
      idle_transports_(),
      mutex_(),
      local_ip_(),
      nat_type_(maidsafe::make_unique<NatType>(NatType::kUnknown))
{
}

ManagedConnections::~ManagedConnections() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto connection_details : connections_)
      connection_details.second->Close();
    connections_.clear();
    for (auto& pending : pendings_)
      pending->pending_transport->Close();
    pendings_.clear();
    for (auto idle_transport : idle_transports_)
      idle_transport->Close();
    idle_transports_.clear();
  }
  asio_service_.Stop();
}

int ManagedConnections::CheckBootstrappingParameters(const BootstrapContacts& bootstrap_list,
                                                     std::shared_ptr<Listener> listener,
                                                     NodeId this_node_id) const {
  if (!listener) {
    LOG(kError) << "You must provide a non-null listener.";
    return kInvalidParameter;
  }
  if (!this_node_id.IsValid()) {
    LOG(kError) << "You must provide a valid node_id.";
    return kInvalidParameter;
  }
  if (bootstrap_list.empty()) {
    LOG(kError) << "You must provide at least one Bootstrap contact.";
    return kNoBootstrapEndpoints;
  }

  return kSuccess;
}

void ManagedConnections::ClearConnectionsAndIdleTransports() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connections_.empty()) {
    for (auto connection_details : connections_) {
      assert(connection_details.second->GetConnection(connection_details.first)->state() ==
             detail::Connection::State::kBootstrapping);
      connection_details.second->Close();
    }
    connections_.clear();
  }
  pendings_.clear();
  for (auto idle_transport : idle_transports_)
    idle_transport->Close();
  idle_transports_.clear();
}

int ManagedConnections::TryToDetermineLocalEndpoint(Endpoint& local_endpoint) {
  bool zero_state(detail::IsValid(local_endpoint));
  if (zero_state) {
    local_ip_ = local_endpoint.address();
  } else {
    local_ip_ = GetLocalIp();
    if (local_ip_.is_unspecified()) {
      LOG(kError) << "Failed to retrieve local IP.";
      return kFailedToGetLocalAddress;
    }
    local_endpoint = Endpoint(local_ip_, 0);
  }
  return kSuccess;
}

void ManagedConnections::AttemptStartNewTransport(const BootstrapContacts& bootstrap_list,
                                                 const Endpoint& local_endpoint,
                                                 const std::function<void(Error, const Contact&)>& handler) {
  StartNewTransport(bootstrap_list, local_endpoint, handler);
}

void ManagedConnections::StartNewTransport(BootstrapContacts bootstrap_list,
                                          Endpoint local_endpoint,
                                          const std::function<void(Error, const Contact&)>& handler) {
  TransportPtr transport(std::make_shared<detail::Transport>(asio_service_, *nat_type_));

  transport->SetManagedConnectionsDebugPrintout([this]() { return DebugString(); });

  bool bootstrap_off_existing_connection(bootstrap_list.empty());
  boost::asio::ip::address external_address;
  if (bootstrap_off_existing_connection)
    GetBootstrapEndpoints(bootstrap_list, external_address);

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Should not bootstrap from the transport belonging to the same routing object
    for (const auto& element : idle_transports_) {
      bootstrap_list.erase(std::remove_if(bootstrap_list.begin(), bootstrap_list.end(),
                                          [&element](const BootstrapContacts::value_type& entry) {
                             return entry.endpoint_pair.local == element->local_endpoint();
                           }),
                           bootstrap_list.end());
    }
  }

  auto on_bootstrap = [=](ReturnCode bootstrap_result, Contact chosen_contact) {
    if (bootstrap_result != kSuccess) {
      transport->Close();
      return handler(RudpErrors::failed_to_bootstrap, chosen_contact);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (chosen_bootstrap_contact_.id.IsValid())
        chosen_bootstrap_contact_ = chosen_contact;
    }

    if (!detail::IsValid(transport->external_endpoint()) && !external_address.is_unspecified()) {
      // Means this node's NAT is symmetric or unknown, so guess that it will be mapped to existing
      // external address and local port.
      transport->SetBestGuessExternalEndpoint(
          Endpoint(external_address, transport->local_endpoint().port()));
    }

    if (bootstrap_result != kSuccess) {
      return handler(RudpErrors::failed_to_bootstrap, chosen_contact);
    }

    return handler(Error(), chosen_contact);
  };

  transport->Bootstrap(
      bootstrap_list, this_node_id_, keys_.public_key, local_endpoint,
      bootstrap_off_existing_connection,
      std::bind(&ManagedConnections::OnMessageSlot, this, args::_1, args::_2),
      [this](const NodeId& peer_id, TransportPtr transport, bool temporary_connection,
             std::atomic<bool>& is_duplicate_normal_connection) {
        OnConnectionAddedSlot(peer_id, transport, temporary_connection,
                              is_duplicate_normal_connection);
      },
      std::bind(&ManagedConnections::OnConnectionLostSlot, this, args::_1, args::_2, args::_3),
      std::bind(&ManagedConnections::OnNatDetectionRequestedSlot, this, args::_1, args::_2,
                args::_3, args::_4),
      on_bootstrap);
}

void ManagedConnections::GetBootstrapEndpoints(BootstrapContacts& bootstrap_list,
                                               boost::asio::ip::address& this_external_address) {
  bool external_address_consistent(true);
  // Favour connections which are on a different network to this to allow calculation of the new
  // transport's external endpoint.
  BootstrapContacts secondary_list;
  bootstrap_list.reserve(Parameters::max_transports * detail::Transport::kMaxConnections());
  secondary_list.reserve(Parameters::max_transports * detail::Transport::kMaxConnections());
  std::set<Endpoint> non_duplicates;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto element : connections_) {
    std::shared_ptr<detail::Connection> connection(element.second->GetConnection(element.first));
    if (!connection)
      continue;
    if (!non_duplicates.insert(connection->Socket().PeerEndpoint()).second)
      continue;  // Already have this endpoint added to bootstrap_contacts or secondary_endpoints.
    Contact peer(connection->Socket().PeerNodeId(), connection->Socket().PeerEndpoint(),
                 connection->Socket().PeerPublicKey());
    if (detail::OnPrivateNetwork(connection->Socket().PeerEndpoint())) {
      secondary_list.push_back(std::move(peer));
    } else {
      bootstrap_list.push_back(std::move(peer));
      Endpoint this_endpoint_as_seen_by_peer(
          element.second->ThisEndpointAsSeenByPeer(element.first));
      if (this_external_address.is_unspecified())
        this_external_address = this_endpoint_as_seen_by_peer.address();
      else if (this_external_address != this_endpoint_as_seen_by_peer.address())
        external_address_consistent = false;
    }
  }
  if (!external_address_consistent)
    this_external_address = boost::asio::ip::address();
  std::random_shuffle(bootstrap_list.begin(), bootstrap_list.end());
  std::random_shuffle(secondary_list.begin(), secondary_list.end());
  bootstrap_list.insert(bootstrap_list.end(), secondary_list.begin(), secondary_list.end());
}

bool ManagedConnections::ExistingConnectionAttempt(const NodeId& peer_id,
                                                   EndpointPair& this_endpoint_pair) const {
  auto existing_attempt(FindPendingTransportWithNodeId(peer_id));
  if (existing_attempt == pendings_.end())
    return false;

  this_endpoint_pair.local = (*existing_attempt)->pending_transport->local_endpoint();
  this_endpoint_pair.external = (*existing_attempt)->pending_transport->external_endpoint();
  assert((*existing_attempt)->pending_transport->IsAvailable());
  return true;
}

bool ManagedConnections::ExistingConnection(const NodeId& peer_id, EndpointPair& this_endpoint_pair,
                                            bool& connection_exists) {
  auto itr(connections_.find(peer_id));
  if (itr == connections_.end())
    return false;

  std::shared_ptr<detail::Connection> connection((*itr).second->GetConnection(peer_id));
  // assert(connection);
  if (!connection) {
    LOG(kError) << "Internal ManagedConnections error: mismatch between connections_ and "
                << "actual connections.";
    connections_.erase(peer_id);
    return false;
  }

  bool bootstrap_connection(connection->state() == detail::Connection::State::kBootstrapping);
  bool unvalidated_connection(connection->state() == detail::Connection::State::kUnvalidated);

  if (bootstrap_connection || unvalidated_connection) {
    this_endpoint_pair.local = (*itr).second->local_endpoint();
    this_endpoint_pair.external = (*itr).second->external_endpoint();
    assert((*itr).second->IsAvailable());
    assert(FindPendingTransportWithNodeId(peer_id) == pendings_.end());
    if (bootstrap_connection) {
      std::unique_ptr<PendingConnection> connection(
          new PendingConnection(peer_id, (*itr).second, asio_service_.service()));
      AddPending(std::move(connection));
    }
    connection_exists = false;
  } else {
    connection_exists = true;
  }
  return true;
}

bool ManagedConnections::SelectIdleTransport(const NodeId& peer_id,
                                             EndpointPair& this_endpoint_pair) {
  while (!idle_transports_.empty()) {
    if ((*idle_transports_.begin())->IsAvailable()) {
      this_endpoint_pair.local = (*idle_transports_.begin())->local_endpoint();
      this_endpoint_pair.external = (*idle_transports_.begin())->external_endpoint();
      assert(FindPendingTransportWithNodeId(peer_id) == pendings_.end());
      std::unique_ptr<PendingConnection> connection(
          new PendingConnection(peer_id, *idle_transports_.begin(), asio_service_.service()));
      AddPending(std::move(connection));
      return true;
    } else {
      idle_transports_.erase(idle_transports_.begin());
    }
  }
  return false;
}

bool ManagedConnections::SelectAnyTransport(const NodeId& peer_id,
                                            EndpointPair& this_endpoint_pair) {
  // Try to get from an existing idle transport (likely to be just-started one).
  if (SelectIdleTransport(peer_id, this_endpoint_pair))
    return true;

  // Get transport with least connections.
  TransportPtr selected_transport(GetAvailableTransport());
  if (!selected_transport)
    return false;

  this_endpoint_pair.local = selected_transport->local_endpoint();
  this_endpoint_pair.external = selected_transport->external_endpoint();
  assert(selected_transport->IsAvailable());
  assert(FindPendingTransportWithNodeId(peer_id) == pendings_.end());
  std::unique_ptr<PendingConnection> connection(
      new PendingConnection(peer_id, selected_transport, asio_service_.service()));
  AddPending(std::move(connection));
  return true;
}

ManagedConnections::TransportPtr ManagedConnections::GetAvailableTransport() const {
  // Get transport with least connections and below kMaxConnections.
  size_t least_connections(detail::Transport::kMaxConnections());
  TransportPtr selected_transport;
  for (auto element : connections_) {
    if (element.second->NormalConnectionsCount() < least_connections) {
      least_connections = element.second->NormalConnectionsCount();
      selected_transport = element.second;
    }
  }
  return selected_transport;
}

bool ManagedConnections::ShouldStartNewTransport(const EndpointPair& peer_endpoint_pair) const {
  bool start_new_transport(false);
  if (*nat_type_ == NatType::kSymmetric &&
      static_cast<int>(connections_.size()) <
          (Parameters::max_transports * detail::Transport::kMaxConnections())) {
    if (detail::IsValid(peer_endpoint_pair.external))
      start_new_transport = true;
    else
      start_new_transport = !detail::IsValid(peer_endpoint_pair.local);
  } else {
    start_new_transport = (static_cast<int>(connections_.size()) < Parameters::max_transports);
  }
  return start_new_transport;
}

void ManagedConnections::AddPending(std::unique_ptr<PendingConnection> connection) {
  NodeId peer_id(connection->node_id);
  pendings_.push_back(std::move(connection));
  pendings_.back()->timer.async_wait([peer_id, this](const boost::system::error_code& ec) {
    if (ec != boost::asio::error::operation_aborted) {
      std::lock_guard<std::mutex> lock(mutex_);
      RemovePending(peer_id);
    }
  });
}

void ManagedConnections::RemovePending(const NodeId& peer_id) {
  auto itr(FindPendingTransportWithNodeId(peer_id));
  if (itr != pendings_.end())
    pendings_.erase(itr);
}

std::vector<std::unique_ptr<ManagedConnections::PendingConnection>>::const_iterator
    ManagedConnections::FindPendingTransportWithNodeId(const NodeId& peer_id) const {
  return std::find_if(pendings_.cbegin(), pendings_.cend(),
                      [&peer_id](const std::unique_ptr<PendingConnection>& element) {
    return element->node_id == peer_id;
  });
}

std::vector<std::unique_ptr<ManagedConnections::PendingConnection>>::iterator
    ManagedConnections::FindPendingTransportWithNodeId(const NodeId& peer_id) {
  return std::find_if(pendings_.begin(), pendings_.end(),
                      [&peer_id](const std::unique_ptr<PendingConnection>& element) {
    return element->node_id == peer_id;
  });
}

void ManagedConnections::DoAdd(const Contact& peer, ConnectionAddedFunctor handler) {
  if (peer.id == this_node_id_) {
    LOG(kError) << "Can't use this node's ID (" << this_node_id_ << ") as peerID.";
    return handler(RudpErrors::operation_not_supported);
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto itr(FindPendingTransportWithNodeId(peer.id));
  if (itr == pendings_.end()) {
    if (connections_.find(peer.id) != connections_.end()) {
      LOG(kWarning) << "A managed connection from " << this_node_id_ << " to " << peer.id
                    << " already exists, and this node's chosen BootstrapID is "
                    << chosen_bootstrap_contact_.id;
      return handler(RudpErrors::already_connected);
    }
    LOG(kError) << "No connection attempt from " << this_node_id_ << " to " << peer.id
                << " - ensure GetAvailableEndpoint has been called first.";
    return handler(RudpErrors::operation_not_supported);
  }

  if ((*itr)->connecting) {
    LOG(kWarning) << "A connection attempt from " << this_node_id_ << " to " << peer.id
                  << " is already happening";
    return handler(RudpErrors::connection_already_in_progress);
  }

  TransportPtr selected_transport((*itr)->pending_transport);
  (*itr)->connecting = true;

  std::shared_ptr<detail::Connection> connection(selected_transport->GetConnection(peer.id));
  if (connection) {
    // If the connection exists, it should be a bootstrapping one.  If the peer used this node,
    // the connection state should be kBootstrapping.  However, if this node bootstrapped off the
    // peer, the peer's validation data will probably already have been received and may have
    // caused the MarkConnectionAsValid to have already been called.  In this case only, the
    // connection will be kPermanent.
    if (connection->state() == detail::Connection::State::kBootstrapping ||
        (chosen_bootstrap_contact_.id == peer.id &&
         connection->state() == detail::Connection::State::kPermanent)) {
      if (connection->state() == detail::Connection::State::kBootstrapping) {
        Endpoint peer_endpoint;
        assert(detail::IsValid(peer_endpoint) ?
                   peer_endpoint == connection->Socket().PeerEndpoint() :
                   true);
      }
      return handler(error_code());
    } else {
      LOG(kError) << "A managed connection from " << this_node_id_ << " to " << peer.id
                  << " already exists, and this node's chosen bootstrap ID is "
                  << chosen_bootstrap_contact_.id;
      pendings_.erase(itr);
      return handler(RudpErrors::already_connected);
    }
  }

  selected_transport->Connect(std::move(peer.id), std::move(peer.endpoint_pair),
                              std::move(peer.public_key), handler);
}

void ManagedConnections::DoRemove(const NodeId& peer_id) {
  if (peer_id == this_node_id_) {
    LOG(kError) << "Can't use this node's ID (" << this_node_id_ << ") as peerID.";
    return;
  }

  TransportPtr transport_to_close;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto itr(connections_.find(peer_id));
    if (itr == connections_.end()) {
      LOG(kWarning) << "Can't remove connection from " << this_node_id_ << " to " << peer_id
                    << " - not in map.";
      return;
    } else {
      transport_to_close = (*itr).second;
    }
  }
  transport_to_close->CloseConnection(peer_id);
}

void ManagedConnections::DoSend(const NodeId& peer_id, SendableMessage&& message,
                                MessageSentFunctor handler) {
  if (peer_id == this_node_id_) {
    LOG(kError) << "Can't use this node's ID (" << this_node_id_ << ") as peerID.";
    return handler(make_error_code(RudpErrors::operation_not_supported));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(connections_.find(peer_id));
  if (itr != connections_.end()) {
    if (itr->second->Send(peer_id, std::string(std::begin(message), std::end(message)), handler)) {
      return;
    }
  }
  LOG(kError) << "Can't send from " << this_node_id_ << " to " << peer_id << " - not in map.";
  if (handler) {
    if (!connections_.empty() || !idle_transports_.empty()) {
      handler(make_error_code(RudpErrors::not_connected));
    } else {
      // Probably haven't bootstrapped, so asio_service_ won't be running.
      std::thread thread(handler, make_error_code(RudpErrors::not_connected));
      thread.detach();
    }
  }
}

void ManagedConnections::OnMessageSlot(const NodeId& peer_id, const std::string& message) {
  try {
    std::string decrypted_message(
#ifdef TESTING
        !Parameters::rudp_encrypt ?
            message :
#endif
            asymm::Decrypt(asymm::CipherText(message), keys_.private_key).string());
    if (auto listener = listener_.lock()) {
      listener->MessageReceived(peer_id, std::vector<unsigned char>(std::begin(decrypted_message),
                                                                    std::end(decrypted_message)));
    }
  } catch (const std::exception& e) {
    LOG(kError) << "Failed to decrypt message: " << e.what();
  }
}

void ManagedConnections::OnConnectionAddedSlot(const NodeId& peer_id, TransportPtr transport,
                                               bool temporary_connection,
                                               std::atomic<bool>& is_duplicate_normal_connection) {
  is_duplicate_normal_connection = false;
  std::lock_guard<std::mutex> lock(mutex_);

  if (temporary_connection) {
    UpdateIdleTransports(transport);
  } else {
    RemovePending(peer_id);

    auto result = connections_.insert(std::make_pair(peer_id, transport));
    bool inserted = result.second;
    is_duplicate_normal_connection = !inserted;

    if (inserted) {
      idle_transports_.erase(transport);
    } else {
      UpdateIdleTransports(transport);

      LOG(kError) << (*result.first).second->ThisDebugId() << " is already connected to " << peer_id
                  << ".  Won't make duplicate normal connection on " << transport->ThisDebugId();
    }
  }

#ifndef NDEBUG
  auto itr(idle_transports_.begin());
  while (itr != idle_transports_.end()) {
    // assert((*itr)->IsIdle());
    if (!(*itr)->IsAvailable())
      itr = idle_transports_.erase(itr);
    else
      ++itr;
  }
#endif
}

void ManagedConnections::UpdateIdleTransports(const TransportPtr& transport) {
  if (transport->IsIdle()) {
    assert(transport->IsAvailable());
    idle_transports_.insert(transport);
  } else {
    idle_transports_.erase(transport);
  }
}

void ManagedConnections::OnConnectionLostSlot(const NodeId& peer_id, TransportPtr transport,
                                              bool temporary_connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpdateIdleTransports(transport);

  if (temporary_connection)
    return;

  // If this is a bootstrap connection, it may have already had GetAvailableEndpoint called on it,
  // but not yet had Add called, in which case peer_id will be in pendings_.  In all other cases,
  // peer_id should not be in pendings_.
  RemovePending(peer_id);

  auto itr(connections_.find(peer_id));
  if (itr != connections_.end()) {
    if ((*itr).second != transport) {
      LOG(kError) << "peer_id: " << peer_id << " is connected via "
                  << (*itr).second->local_endpoint() << " not " << transport->local_endpoint();
      BOOST_ASSERT(false);
    }

    connections_.erase(itr);

    if (peer_id == chosen_bootstrap_contact_.id)
      chosen_bootstrap_contact_ = Contact();

    if (auto listener = listener_.lock())
      listener->ConnectionLost(peer_id);
  }
}

void ManagedConnections::OnNatDetectionRequestedSlot(const Endpoint& this_local_endpoint,
                                                     const NodeId& peer_id,
                                                     const Endpoint& peer_endpoint,
                                                     uint16_t& another_external_port) {
  if (*nat_type_ == NatType::kUnknown || *nat_type_ == NatType::kSymmetric) {
    another_external_port = 0;
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(std::find_if(connections_.begin(), connections_.end(),
                        [&this_local_endpoint](const ConnectionMap::value_type& element) {
    return this_local_endpoint != element.second->local_endpoint();
  }));

  if (itr == connections_.end()) {
    another_external_port = 0;
    return;
  }

  another_external_port = (*itr).second->external_endpoint().port();
  // This node doesn't care about the Ping result, but Ping should not be given a NULL functor.
  assert(0 && "FIXME: valid public key");
  (*itr).second->Ping(peer_id, peer_endpoint, asymm::PublicKey(), [](int) {});  // NOLINT (Fraser)
}

std::string ManagedConnections::DebugString() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Not interested in the log once accumulated enough connections
  if (connections_.size() > 8)
    return "";

  //  std::string s = "This node's peer connections:\n";
  std::set<TransportPtr> transports;
  for (auto connection : connections_) {
    transports.insert(connection.second);
    //     s += '\t' + DebugId(connection.first).substr(0, 7) + '\n';
  }

  std::string s = "This node's own transports and their peer connections:\n";
  for (auto transport : transports)
    s += transport->DebugString();

  s += "\nThis node's idle transports:\n";
  for (auto idle_transport : idle_transports_)
    s += idle_transport->DebugString();

  s += "\nThis node's pending connections:\n";
  for (auto& pending : pendings_) {
    s += "\tPending to peer " + DebugId(pending->node_id).substr(0, 7);
    s += " on this node's transport ";
    s += boost::lexical_cast<std::string>(pending->pending_transport->external_endpoint()) + " / ";
    s += boost::lexical_cast<std::string>(pending->pending_transport->local_endpoint()) + '\n';
  }
  s += "\n\n";

  return s;
}

}  // namespace rudp

}  // namespace maidsafe
