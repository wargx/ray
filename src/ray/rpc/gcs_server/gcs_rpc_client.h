// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "ray/common/network_util.h"
#include "ray/rpc/grpc_client.h"
#include "src/ray/protobuf/gcs_service.grpc.pb.h"

namespace ray {
namespace rpc {

class GcsRpcClient;

/// \class Executor
/// Executor saves operation and support retries.
class Executor {
 public:
  explicit Executor(GcsRpcClient *gcs_rpc_client) : gcs_rpc_client_(gcs_rpc_client) {}

  /// This function is used to execute the given operation.
  ///
  /// \param operation The operation to be executed.
  void Execute(const std::function<void(GcsRpcClient *gcs_rpc_client)> &operation) {
    operation_ = operation;
    operation(gcs_rpc_client_);
  }

  /// This function is used to retry the given operation.
  void Retry() { operation_(gcs_rpc_client_); }

 private:
  GcsRpcClient *gcs_rpc_client_;
  std::function<void(GcsRpcClient *gcs_rpc_client)> operation_;
};

/// Define a void GCS RPC client method.
///
/// Example:
///   VOID_GCS_RPC_CLIENT_METHOD(
///     ActorInfoGcsService,
///     CreateActor,
///     actor_info_grpc_client_,
///     /*method_timeout_ms*/ -1,) # Default value
///   generates
///
///     # Asynchronous RPC. Callback will be invoked once the RPC is replied.
///     rpc_client_.CreateActor(request, callback, timeout_ms = -1);
///
///     # Synchronous RPC. The function will return once the RPC is replied.
///     rpc_client_.SyncCreateActor(request, *reply, timeout_ms = -1);
///
/// Retry protocol:
///   Currently, Ray assumes the GCS server is HA.
///   That says, when there's any RPC failure, the method will automatically retry
///   under the hood.
///
/// \param SERVICE name of the service.
/// \param METHOD name of the RPC method.
/// \param grpc_client The grpc client to invoke RPC.
/// \param method_timeout_ms The RPC timeout in ms. If the RPC times out,
/// it will return status::TimedOut. Timeout can be configured in 3 levels;
/// whole service, handler, and each call.
/// The priority of timeout is each call > handler > whole service
/// (the lower priority timeout is overwritten by the higher priority timeout).
/// \param SPECS The cpp method spec. For example, override.
///
/// Currently, SyncMETHOD will copy the reply additionally.
/// TODO(sang): Fix it.
#define VOID_GCS_RPC_CLIENT_METHOD(                                                    \
    SERVICE, METHOD, grpc_client, method_timeout_ms, SPECS)                            \
  void METHOD(const METHOD##Request &request,                                          \
              const ClientCallback<METHOD##Reply> &callback,                           \
              const int64_t timeout_ms = method_timeout_ms) SPECS {                    \
    auto executor = new Executor(this);                                                \
    auto operation_callback = [this, request, callback, executor](                     \
                                  const ray::Status &status,                           \
                                  const METHOD##Reply &reply) {                        \
      if (status.IsTimedOut()) {                                                       \
        callback(status, reply);                                                       \
        delete executor;                                                               \
      } else if (!status.IsGrpcError()) {                                              \
        auto status =                                                                  \
            reply.status().code() == (int)StatusCode::OK                               \
                ? Status()                                                             \
                : Status(StatusCode(reply.status().code()), reply.status().message()); \
        callback(status, reply);                                                       \
        delete executor;                                                               \
      } else if (gcs_is_down_) {                                                       \
        queued_executors_.emplace_back(executor);                                      \
      } else {                                                                         \
        executor->Retry();                                                             \
      }                                                                                \
    };                                                                                 \
    auto operation =                                                                   \
        [request, operation_callback, timeout_ms](GcsRpcClient *gcs_rpc_client) {      \
          RAY_UNUSED(INVOKE_RPC_CALL(SERVICE,                                          \
                                     METHOD,                                           \
                                     request,                                          \
                                     operation_callback,                               \
                                     gcs_rpc_client->grpc_client,                      \
                                     timeout_ms));                                     \
        };                                                                             \
    executor->Execute(operation);                                                      \
  }                                                                                    \
                                                                                       \
  ray::Status Sync##METHOD(const METHOD##Request &request,                             \
                           METHOD##Reply *reply_in,                                    \
                           const int64_t timeout_ms = method_timeout_ms) {             \
    std::promise<Status> promise;                                                      \
    METHOD(                                                                            \
        request,                                                                       \
        [&promise, reply_in](const Status &status, const METHOD##Reply &reply) {       \
          reply_in->CopyFrom(reply);                                                   \
          promise.set_value(status);                                                   \
        },                                                                             \
        timeout_ms);                                                                   \
    return promise.get_future().get();                                                 \
  }
/// Client used for communicating with gcs server.
class GcsRpcClient {
 public:
  /// Constructor.
  ///
  /// \param[in] address Address of gcs server.
  /// \param[in] port Port of the gcs server.
  /// \param[in] client_call_manager The `ClientCallManager` used for managing requests.
  /// \param[in] gcs_service_failure_detected The function is used to redo subscription
  /// and reconnect to GCS RPC server when gcs service failure is detected.
  GcsRpcClient(const std::string &address,
               const int port,
               ClientCallManager &client_call_manager) {
    Reset(address, port, client_call_manager);
  };

  void Reset(const std::string &address,
             const int port,
             ClientCallManager &client_call_manager) {
    io_context_ = &client_call_manager.GetMainService();
    periodical_runner_ = std::make_unique<PeriodicalRunner>(*io_context_);
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS,
                     RayConfig::instance().gcs_grpc_max_reconnect_backoff_ms());
    arguments.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS,
                     RayConfig::instance().gcs_grpc_min_reconnect_backoff_ms());
    arguments.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS,
                     RayConfig::instance().gcs_grpc_initial_reconnect_backoff_ms());

    channel_ = BuildChannel(address, port, arguments);
    job_info_grpc_client_ =
        std::make_unique<GrpcClient<JobInfoGcsService>>(channel_, client_call_manager);
    actor_info_grpc_client_ =
        std::make_unique<GrpcClient<ActorInfoGcsService>>(channel_, client_call_manager);
    node_info_grpc_client_ =
        std::make_unique<GrpcClient<NodeInfoGcsService>>(channel_, client_call_manager);
    node_resource_info_grpc_client_ =
        std::make_unique<GrpcClient<NodeResourceInfoGcsService>>(channel_,
                                                                 client_call_manager);
    heartbeat_info_grpc_client_ = std::make_unique<GrpcClient<HeartbeatInfoGcsService>>(
        channel_, client_call_manager);
    stats_grpc_client_ =
        std::make_unique<GrpcClient<StatsGcsService>>(channel_, client_call_manager);
    worker_info_grpc_client_ =
        std::make_unique<GrpcClient<WorkerInfoGcsService>>(channel_, client_call_manager);
    placement_group_info_grpc_client_ =
        std::make_unique<GrpcClient<PlacementGroupInfoGcsService>>(channel_,
                                                                   client_call_manager);
    internal_kv_grpc_client_ =
        std::make_unique<GrpcClient<InternalKVGcsService>>(channel_, client_call_manager);
    internal_pubsub_grpc_client_ = std::make_unique<GrpcClient<InternalPubSubGcsService>>(
        channel_, client_call_manager);

    // Setup monitor for gRPC channel status.
    // TODO(iycheng): Push this into ClientCallManager with CQ by using async call.
    periodical_runner_->RunFnPeriodically(
        [this] {
      auto status = channel_->GetState();
      switch (status) {
      case GRPC_CHANNEL_TRANSIENT_FAILURE:
        RAY_CHECK(
            absl::ToInt64Seconds(gcs_last_alive_time_ - absl::Now()) <
            absl::Seconds(::RayConfig::instance().gcs_rpc_server_reconnect_timeout_s()))
            << "Failed to connect to GCS within "
            << RayConfig::instance().gcs_rpc_server_reconnect_timeout_s() << " seconds";
        gcs_is_down_ = true;
        break;
      case SHUTDOWN:
        RAY_CHECK(shutdown_) << "Channel shoud never go to this status.";
        break;
      case CONNECTING:
      case READY:
      case IDLE:
        gcs_last_alive_time_ = absl::Now();
        gcs_is_down_ = false;
        // Retry the one queued.
        while (!queued_executors_.empty()) {
          queued_executors_.back()->Retry();
          queued_executors_.pop_back();
        }
        break;
      default:
        RAY_CHECK(false) << "Not covered status: " << status;
      }
      if (status == TRANSIENT_FAILURE) {
        gcs_is_down_ = true;
      },
        RayConfig::instance().gcs_client_check_connection_status_interval_milliseconds());
  }

  /// Add job info to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(JobInfoGcsService,
                             AddJob,
                             job_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Mark job as finished to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(JobInfoGcsService,
                             MarkJobFinished,
                             job_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get information of all jobs from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(JobInfoGcsService,
                             GetAllJobInfo,
                             job_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Report job error to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(JobInfoGcsService,
                             ReportJobError,
                             job_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get next job id from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(JobInfoGcsService,
                             GetNextJobID,
                             job_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Register actor via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             RegisterActor,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Create actor via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             CreateActor,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get actor data from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             GetActorInfo,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get actor data from GCS Service by name.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             GetNamedActorInfo,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get all named actor names from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             ListNamedActors,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get all actor data from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             GetAllActorInfo,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Kill actor via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(ActorInfoGcsService,
                             KillActorViaGcs,
                             actor_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Register a node to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeInfoGcsService,
                             RegisterNode,
                             node_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Unregister a node from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeInfoGcsService,
                             DrainNode,
                             node_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get information of all nodes from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeInfoGcsService,
                             GetAllNodeInfo,
                             node_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get internal config of the node from the GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeInfoGcsService,
                             GetInternalConfig,
                             node_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get node's resources from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeResourceInfoGcsService,
                             GetResources,
                             node_resource_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get available resources of all nodes from the GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeResourceInfoGcsService,
                             GetAllAvailableResources,
                             node_resource_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Report resource usage of a node to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeResourceInfoGcsService,
                             ReportResourceUsage,
                             node_resource_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get resource usage of all nodes from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(NodeResourceInfoGcsService,
                             GetAllResourceUsage,
                             node_resource_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Report heartbeat of a node to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(HeartbeatInfoGcsService,
                             ReportHeartbeat,
                             heartbeat_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Check GCS is alive.
  VOID_GCS_RPC_CLIENT_METHOD(HeartbeatInfoGcsService,
                             CheckAlive,
                             heartbeat_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Add profile data to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(StatsGcsService,
                             AddProfileData,
                             stats_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get information of all profiles from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(StatsGcsService,
                             GetAllProfileInfo,
                             stats_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Report a worker failure to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(WorkerInfoGcsService,
                             ReportWorkerFailure,
                             worker_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get worker information from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(WorkerInfoGcsService,
                             GetWorkerInfo,
                             worker_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get information of all workers from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(WorkerInfoGcsService,
                             GetAllWorkerInfo,
                             worker_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Add worker information to GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(WorkerInfoGcsService,
                             AddWorkerInfo,
                             worker_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Create placement group via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             CreatePlacementGroup,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Remove placement group via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             RemovePlacementGroup,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  /// Get placement group via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             GetPlacementGroup,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get placement group data from GCS Service by name.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             GetNamedPlacementGroup,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Get information of all placement group from GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             GetAllPlacementGroup,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Wait for placement group until ready via GCS Service.
  VOID_GCS_RPC_CLIENT_METHOD(PlacementGroupInfoGcsService,
                             WaitPlacementGroupUntilReady,
                             placement_group_info_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Operations for kv (Get, Put, Del, Exists)
  VOID_GCS_RPC_CLIENT_METHOD(InternalKVGcsService,
                             InternalKVGet,
                             internal_kv_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalKVGcsService,
                             InternalKVPut,
                             internal_kv_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalKVGcsService,
                             InternalKVDel,
                             internal_kv_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalKVGcsService,
                             InternalKVExists,
                             internal_kv_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalKVGcsService,
                             InternalKVKeys,
                             internal_kv_grpc_client_,
                             /*method_timeout_ms*/ -1, )

  /// Operations for pubsub
  VOID_GCS_RPC_CLIENT_METHOD(InternalPubSubGcsService,
                             GcsPublish,
                             internal_pubsub_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalPubSubGcsService,
                             GcsSubscriberPoll,
                             internal_pubsub_grpc_client_,
                             /*method_timeout_ms*/ -1, )
  VOID_GCS_RPC_CLIENT_METHOD(InternalPubSubGcsService,
                             GcsSubscriberCommandBatch,
                             internal_pubsub_grpc_client_,
                             /*method_timeout_ms*/ -1, )


   void Shutdown() {
      shutdown_ = true;
      periodical_runner_.reset();
   }
 private:
  void ReconnectHelper(absl::Time deadline,
                       std::shared_ptr<boost::asio::deadline_timer> timer) {
      if (absl::Now() > deadline) {
        RAY_LOG(FATAL) << "Fail to reconnect to GCS";
      }
      if (gcs_service_failure_detected_(GcsServiceFailureType::RPC_DISCONNECT)) {
        gcs_is_down_ = false;
        while (!queued_executors_.empty()) {
          auto e = queued_executors_.back();
          e->Retry();
          queued_executors_.pop_back();
        }
      } else {
        RAY_LOG(INFO) << "Try to reconnect to GCS failed, "
                      << absl::ToInt64Seconds(deadline - absl::Now()) << " seconds left";
        timer->expires_from_now(boost::posix_time::seconds(1));
        timer->async_wait([this, deadline, timer](const auto &ec) {
          ReconnectHelper(deadline, timer);
        });
      }
  }

  instrumented_io_context *io_context_;

  /// The gRPC-generated stub.
  std::unique_ptr<GrpcClient<JobInfoGcsService>> job_info_grpc_client_;
  std::unique_ptr<GrpcClient<ActorInfoGcsService>> actor_info_grpc_client_;
  std::unique_ptr<GrpcClient<NodeInfoGcsService>> node_info_grpc_client_;
  std::unique_ptr<GrpcClient<NodeResourceInfoGcsService>> node_resource_info_grpc_client_;
  std::unique_ptr<GrpcClient<HeartbeatInfoGcsService>> heartbeat_info_grpc_client_;
  std::unique_ptr<GrpcClient<StatsGcsService>> stats_grpc_client_;
  std::unique_ptr<GrpcClient<WorkerInfoGcsService>> worker_info_grpc_client_;
  std::unique_ptr<GrpcClient<PlacementGroupInfoGcsService>>
      placement_group_info_grpc_client_;
  std::unique_ptr<GrpcClient<InternalKVGcsService>> internal_kv_grpc_client_;
  std::unique_ptr<GrpcClient<InternalPubSubGcsService>> internal_pubsub_grpc_client_;

  std::shared_ptr<grpc::Channel> channel_;
  bool gcs_is_down_ = false;
  absl::Time gcs_last_alive_time_;
  bool shutdown_ = false;
  std::unique_ptr<PeriodicalRunner> periodical_runner_;
  std::vector<Executor *> queued_executors_;
  };

}  // namespace rpc
}  // namespace ray
