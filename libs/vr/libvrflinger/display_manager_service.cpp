#include "display_manager_service.h"

#include <android-base/file.h>
#include <android-base/properties.h>
#include <pdx/channel_handle.h>
#include <pdx/default_transport/service_endpoint.h>
#include <private/android_filesystem_config.h>
#include <private/dvr/display_protocol.h>
#include <private/dvr/trusted_uids.h>
#include <sys/poll.h>

#include <array>

using android::dvr::display::DisplayManagerProtocol;
using android::pdx::Channel;
using android::pdx::LocalChannelHandle;
using android::pdx::Message;
using android::pdx::default_transport::Endpoint;
using android::pdx::ErrorStatus;
using android::pdx::rpc::DispatchRemoteMethod;
using android::pdx::rpc::IfAnyOf;
using android::pdx::rpc::RemoteMethodError;

namespace {

const char kDvrLensMetricsProperty[] = "ro.dvr.lens_metrics";
const char kDvrDeviceMetricsProperty[] = "ro.dvr.device_metrics";
const char kDvrDeviceConfigProperty[] = "ro.dvr.device_configuration";

}  // namespace

namespace android {
namespace dvr {

void DisplayManager::SetNotificationsPending(bool pending) {
  auto status = service_->ModifyChannelEvents(channel_id_, pending ? 0 : POLLIN,
                                              pending ? POLLIN : 0);
  ALOGE_IF(!status,
           "DisplayManager::SetNotificationPending: Failed to modify channel "
           "events: %s",
           status.GetErrorMessage().c_str());
}

DisplayManagerService::DisplayManagerService(
    const std::shared_ptr<DisplayService>& display_service)
    : BASE("DisplayManagerService",
           Endpoint::Create(DisplayManagerProtocol::kClientPath)),
      display_service_(display_service) {
  display_service_->SetDisplayConfigurationUpdateNotifier(
      std::bind(&DisplayManagerService::OnDisplaySurfaceChange, this));
}

std::shared_ptr<pdx::Channel> DisplayManagerService::OnChannelOpen(
    pdx::Message& message) {
  const int user_id = message.GetEffectiveUserId();
  const bool trusted = user_id == AID_ROOT || IsTrustedUid(user_id);

  // Prevent more than one display manager from registering at a time or
  // untrusted UIDs from connecting.
  if (display_manager_ || !trusted) {
    RemoteMethodError(message, EPERM);
    return nullptr;
  }

  display_manager_ =
      std::make_shared<DisplayManager>(this, message.GetChannelId());
  return display_manager_;
}

void DisplayManagerService::OnChannelClose(
    pdx::Message& /*message*/, const std::shared_ptr<pdx::Channel>& channel) {
  // Unregister the display manager when the channel closes.
  if (display_manager_ == channel)
    display_manager_ = nullptr;
}

pdx::Status<void> DisplayManagerService::HandleMessage(pdx::Message& message) {
  auto channel = std::static_pointer_cast<DisplayManager>(message.GetChannel());

  switch (message.GetOp()) {
    case DisplayManagerProtocol::GetSurfaceState::Opcode:
      DispatchRemoteMethod<DisplayManagerProtocol::GetSurfaceState>(
          *this, &DisplayManagerService::OnGetSurfaceState, message);
      return {};

    case DisplayManagerProtocol::GetSurfaceQueue::Opcode:
      DispatchRemoteMethod<DisplayManagerProtocol::GetSurfaceQueue>(
          *this, &DisplayManagerService::OnGetSurfaceQueue, message);
      return {};

    case DisplayManagerProtocol::SetupNamedBuffer::Opcode:
      DispatchRemoteMethod<DisplayManagerProtocol::SetupNamedBuffer>(
          *this, &DisplayManagerService::OnSetupNamedBuffer, message);
      return {};

    case DisplayManagerProtocol::GetConfigurationData::Opcode:
      DispatchRemoteMethod<DisplayManagerProtocol::GetConfigurationData>(
          *this, &DisplayManagerService::OnGetConfigurationData, message);
      return {};

    default:
      return Service::DefaultHandleMessage(message);
  }
}

pdx::Status<std::vector<display::SurfaceState>>
DisplayManagerService::OnGetSurfaceState(pdx::Message& /*message*/) {
  std::vector<display::SurfaceState> items;

  display_service_->ForEachDisplaySurface(
      SurfaceType::Application,
      [&items](const std::shared_ptr<DisplaySurface>& surface) mutable {
        items.push_back({surface->surface_id(), surface->process_id(),
                         surface->user_id(), surface->attributes(),
                         surface->update_flags(), surface->GetQueueIds()});
        surface->ClearUpdate();
      });

  // The fact that we're in the message handler implies that display_manager_ is
  // not nullptr. No check required, unless this service becomes multi-threaded.
  display_manager_->SetNotificationsPending(false);
  return items;
}

pdx::Status<pdx::LocalChannelHandle> DisplayManagerService::OnGetSurfaceQueue(
    pdx::Message& /*message*/, int surface_id, int queue_id) {
  auto surface = display_service_->GetDisplaySurface(surface_id);
  if (!surface || surface->surface_type() != SurfaceType::Application)
    return ErrorStatus(EINVAL);

  auto queue =
      std::static_pointer_cast<ApplicationDisplaySurface>(surface)->GetQueue(
          queue_id);
  if (!queue)
    return ErrorStatus(EINVAL);

  auto status = queue->CreateConsumerQueueHandle();
  ALOGE_IF(
      !status,
      "DisplayManagerService::OnGetSurfaceQueue: Failed to create consumer "
      "queue for queue_id=%d: %s",
      queue->id(), status.GetErrorMessage().c_str());

  return status;
}

pdx::Status<BorrowedNativeBufferHandle>
DisplayManagerService::OnSetupNamedBuffer(pdx::Message& message,
                                          const std::string& name, size_t size,
                                          uint64_t usage) {
  const int user_id = message.GetEffectiveUserId();
  const bool trusted = user_id == AID_ROOT || IsTrustedUid(user_id);

  if (!trusted) {
    ALOGE(
        "DisplayService::SetupNamedBuffer: Named buffers may only be created "
        "by trusted UIDs: user_id=%d",
        user_id);
    return ErrorStatus(EPERM);
  }
  return display_service_->SetupNamedBuffer(name, size, usage);
}

pdx::Status<std::string> DisplayManagerService::OnGetConfigurationData(
    pdx::Message& message, display::ConfigFileType config_type) {
  std::string property_name;
  switch (config_type) {
    case display::ConfigFileType::kLensMetrics:
      property_name = kDvrLensMetricsProperty;
      break;
    case display::ConfigFileType::kDeviceMetrics:
      property_name = kDvrDeviceMetricsProperty;
      break;
    case display::ConfigFileType::kDeviceConfiguration:
      property_name = kDvrDeviceConfigProperty;
      break;
    default:
      return ErrorStatus(EINVAL);
  }
  std::string file_path = base::GetProperty(property_name, "");
  if (file_path.empty()) {
    return ErrorStatus(ENOENT);
  }

  std::string data;
  if (!base::ReadFileToString(file_path, &data)) {
    return ErrorStatus(errno);
  }

  return std::move(data);
}

void DisplayManagerService::OnDisplaySurfaceChange() {
  if (display_manager_)
    display_manager_->SetNotificationsPending(true);
}

}  // namespace dvr
}  // namespace android
