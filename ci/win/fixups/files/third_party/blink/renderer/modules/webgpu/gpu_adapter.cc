// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_adapter.h"

#include <algorithm>

#include "services/metrics/public/cpp/ukm_builders.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_object_builder.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_device_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_queue_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_request_adapter_options.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/modules/webgpu/dawn_enum_conversions.h"
#include "third_party/blink/renderer/modules/webgpu/gpu.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_adapter_info.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device_lost_info.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_memory_heap_info.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_subgroup_matrix_config.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_supported_features.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_supported_limits.h"
#include "third_party/blink/renderer/modules/webgpu/string_utils.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_callback.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/wtf/text/format.h"

// MZ: 引入指纹配置接口
#include "mz/fingerprint/browser_fingerprint.h"

namespace blink {

namespace {

// MZ: 从 WebGL renderer 字符串解析 GPU 信息
struct ParsedGPUInfo {
  std::string vendor;        // 例如 "nvidia", "intel", "amd"
  std::string description;   // 例如 "NVIDIA GeForce RTX 4080"
  std::string architecture;  // 例如 "ada", "ampere", "xe-lpg"
};

// MZ: 根据 GPU 型号推断架构。名字必须和 Dawn 上报的一致（gpu_info / dawn
// GetArchitecture 的取值），否则写一个真实 Chrome 不会出现的值反而更显眼。
// 推断不出来时返回空串：调用方会把架构置空，不能留着真实显卡的架构。
bool MzContains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string InferArchitecture(const std::string& vendor,
                              const std::string& description) {
  if (vendor == "nvidia") {
    // 先匹配新卡：RTX 50 → Blackwell，RTX 40 → Ada（Dawn 叫 lovelace）
    if (MzContains(description, "RTX 50") || MzContains(description, "RTX PRO")) {
      return "blackwell";
    }
    if (MzContains(description, "RTX 40") || MzContains(description, "RTX 2000 Ada") ||
        MzContains(description, "RTX 4000 Ada") || MzContains(description, "RTX 6000 Ada")) {
      return "lovelace";
    }
    if (MzContains(description, "RTX 30") || MzContains(description, "RTX A") ||
        MzContains(description, "MX5") || MzContains(description, "MX4")) {
      return "ampere";
    }
    if (MzContains(description, "RTX 20") || MzContains(description, "GTX 16") ||
        MzContains(description, "MX2") || MzContains(description, "TITAN RTX") ||
        MzContains(description, "Quadro RTX") || MzContains(description, "T1000") ||
        MzContains(description, "T600") || MzContains(description, "T400")) {
      return "turing";
    }
    if (MzContains(description, "GTX 10") || MzContains(description, "MX1") ||
        MzContains(description, "MX3") || MzContains(description, "Quadro P") ||
        MzContains(description, "TITAN X") || MzContains(description, "GT 10")) {
      return "pascal";
    }
    if (MzContains(description, "GTX 9") || MzContains(description, "GTX 750") ||
        MzContains(description, "Quadro M")) {
      return "maxwell";
    }
    return "";
  }

  if (vendor == "intel") {
    // Arc B 系（Battlemage）/ Lunar Lake 140V → xe-2；Arc A 系 → xe-hpg；
    // Meteor/Arrow Lake 的 "Arc Graphics" 核显 → xe-lpg；Iris Xe / UHD 7xx → gen-12lp
    if (MzContains(description, "Arc(TM) B") || MzContains(description, "Arc B") ||
        MzContains(description, "140V") ||
        MzContains(description, "130V")) {
      return "xe-2lpg";
    }
    if (MzContains(description, "Arc(TM) A") || MzContains(description, "Arc A")) {
      return "xe-hpg";
    }
    if (MzContains(description, "Arc")) {
      return "xe-lpg";
    }
    if (MzContains(description, "Iris(R) Xe") || MzContains(description, "Iris Xe") ||
        MzContains(description, "UHD Graphics 7") || MzContains(description, "UHD Graphics 770") ||
        MzContains(description, "UHD Graphics 730") || MzContains(description, "UHD Graphics 710") ||
        MzContains(description, "UHD Graphics Family") || MzContains(description, "Graphics (")) {
      return "gen-12lp";
    }
    if (MzContains(description, "UHD Graphics 6") || MzContains(description, "Iris(R) Plus") ||
        MzContains(description, "Iris Plus")) {
      // UHD 6xx（Coffee/Comet Lake）是 gen-9；Ice Lake 的 Iris Plus 是 gen-11
      return MzContains(description, "Iris") ? "gen-11" : "gen-9";
    }
    if (MzContains(description, "HD Graphics 5") || MzContains(description, "HD Graphics 6")) {
      return "gen-9";
    }
    return "";
  }

  if (vendor == "amd") {
    if (MzContains(description, "RX 9")) {
      return "rdna-4";
    }
    if (MzContains(description, "RX 7") || MzContains(description, "Radeon 7") ||
        MzContains(description, "Radeon 8")) {
      // RX 7000 独显、Radeon 780M/880M/890M 核显都是 RDNA3/3.5，Dawn 统一报 rdna-3
      return "rdna-3";
    }
    if (MzContains(description, "RX 6") || MzContains(description, "Radeon 6")) {
      return "rdna-2";
    }
    if (MzContains(description, "RX 5")) {
      // RX 5000 是 RDNA1；RX 580/570 之类三位数的是 GCN4
      return MzContains(description, "RX 5700") || MzContains(description, "RX 5600") ||
                     MzContains(description, "RX 5500") || MzContains(description, "RX 5300")
                 ? "rdna-1"
                 : "gcn-4";
    }
    if (MzContains(description, "Vega") || MzContains(description, "Radeon(TM) Graphics") ||
        MzContains(description, "Radeon Graphics")) {
      return "gcn-5";
    }
    return "";
  }

  return "";
}

// MZ: 不同厂商的子组大小（subgroupMinSize/subgroupMaxSize）不同，
// 和伪装的显卡对不上同样会露馅。取值为 D3D12 下各家驱动的 WaveLaneCountMin/Max。
bool InferSubgroupSizes(const std::string& vendor,
                        const std::string& architecture,
                        uint32_t* min_size,
                        uint32_t* max_size) {
  if (vendor == "nvidia") {
    *min_size = 32;
    *max_size = 32;
    return true;
  }
  if (vendor == "amd") {
    // RDNA 支持 wave32/wave64，GCN 只有 wave64
    const bool gcn = architecture.rfind("gcn", 0) == 0;
    *min_size = gcn ? 64 : 32;
    *max_size = 64;
    return true;
  }
  if (vendor == "intel") {
    *min_size = architecture.rfind("xe-2", 0) == 0 ? 16 : 8;
    *max_size = 32;
    return true;
  }
  return false;
}

// MZ: 解析 WebGL renderer 字符串
// 输入示例: "ANGLE (NVIDIA, NVIDIA GeForce RTX 4080 Direct3D11 vs_5_0 ps_5_0, D3D11)"
// 或: "Google Inc. (NVIDIA)"
ParsedGPUInfo ParseWebGLRenderer(const std::string& vendor_str, const std::string& renderer_str) {
  ParsedGPUInfo info;

  // 解析 vendor
  // "Google Inc. (NVIDIA)" -> "nvidia"
  size_t vendor_start = vendor_str.find('(');
  size_t vendor_end = vendor_str.find(')');
  if (vendor_start != std::string::npos && vendor_end != std::string::npos) {
    std::string vendor_name = vendor_str.substr(vendor_start + 1, vendor_end - vendor_start - 1);
    // 转换为小写
    std::transform(vendor_name.begin(), vendor_name.end(), vendor_name.begin(), ::tolower);
    info.vendor = vendor_name;
  } else {
    // 如果没有括号，直接使用整个字符串
    std::string vendor_name = vendor_str;
    std::transform(vendor_name.begin(), vendor_name.end(), vendor_name.begin(), ::tolower);
    info.vendor = vendor_name;
  }

  // 解析 renderer
  // "ANGLE (NVIDIA, NVIDIA GeForce RTX 4080 Direct3D11 vs_5_0 ps_5_0, D3D11)"
  // 提取 "NVIDIA GeForce RTX 4080"
  size_t angle_start = renderer_str.find("ANGLE (");
  if (angle_start != std::string::npos) {
    size_t comma1 = renderer_str.find(',', angle_start);
    size_t comma2 = renderer_str.find(',', comma1 + 1);
    if (comma1 != std::string::npos && comma2 != std::string::npos) {
      std::string gpu_part = renderer_str.substr(comma1 + 1, comma2 - comma1 - 1);
      // 去除前后空格
      size_t start = gpu_part.find_first_not_of(" \t");
      size_t end = gpu_part.find_last_not_of(" \t");
      if (start != std::string::npos && end != std::string::npos) {
        gpu_part = gpu_part.substr(start, end - start + 1);
      }
      // 移除 "Direct3D11 vs_5_0 ps_5_0" 等后缀
      size_t direct3d_pos = gpu_part.find("Direct3D");
      if (direct3d_pos != std::string::npos) {
        gpu_part = gpu_part.substr(0, direct3d_pos);
        // 再次去除尾部空格
        end = gpu_part.find_last_not_of(" \t");
        if (end != std::string::npos) {
          gpu_part = gpu_part.substr(0, end + 1);
        }
      }
      info.description = gpu_part;
    }
  } else {
    // 如果不是 ANGLE 格式，直接使用 renderer 字符串
    info.description = renderer_str;
  }

  // 推断架构
  info.architecture = InferArchitecture(info.vendor, info.description);

  return info;
}

GPUSupportedFeatures* MakeFeatureNameSet(wgpu::Adapter adapter) {
  GPUSupportedFeatures* features = MakeGarbageCollected<GPUSupportedFeatures>();
  DCHECK(features->FeatureNameSet().empty());

  wgpu::SupportedFeatures supported_features;
  adapter.GetFeatures(&supported_features);
  // SAFETY: Required from caller
  const auto features_span = UNSAFE_BUFFERS(base::span<const wgpu::FeatureName>(
      supported_features.features, supported_features.featureCount));
  for (const auto& f : features_span) {
    auto feature_name_enum_optional =
        GPUSupportedFeatures::ToV8FeatureNameEnum(f);
    if (feature_name_enum_optional) {
      features->AddFeatureName(
          V8GPUFeatureName(feature_name_enum_optional.value()));
      }
  }
  return features;
}

}  // anonymous namespace

GPUAdapter::GPUAdapter(
    GPU* gpu,
    wgpu::Adapter handle,
    scoped_refptr<DawnControlClientHolder> dawn_control_client,
    const GPURequestAdapterOptions* options)
    : DawnObject(dawn_control_client, std::move(handle), String()), gpu_(gpu) {
  wgpu::AdapterInfo info = {};
  wgpu::ChainedStructOut** propertiesChain = &info.nextInChain;
  if (GetHandle().HasFeature(wgpu::FeatureName::AdapterPropertiesMemoryHeaps)) {
    *propertiesChain = &memory_heaps_;
    propertiesChain = &(*propertiesChain)->nextInChain;
  }
  if (GetHandle().HasFeature(
          wgpu::FeatureName::ChromiumExperimentalSubgroupMatrix)) {
    *propertiesChain = &subgroup_matrix_configs_;
    propertiesChain = &(*propertiesChain)->nextInChain;
  }
  wgpu::AdapterPropertiesD3D d3dProperties = {};
  bool supportsPropertiesD3D =
      GetHandle().HasFeature(wgpu::FeatureName::AdapterPropertiesD3D);
  if (supportsPropertiesD3D) {
    *propertiesChain = &d3dProperties;
    propertiesChain = &(*propertiesChain)->nextInChain;
  }
  wgpu::AdapterPropertiesVk vkProperties = {};
  bool supportsPropertiesVk =
      GetHandle().HasFeature(wgpu::FeatureName::AdapterPropertiesVk);
  if (supportsPropertiesVk) {
    *propertiesChain = &vkProperties;
    propertiesChain = &(*propertiesChain)->nextInChain;
  }
  wgpu::DawnAdapterPropertiesPowerPreference powerProperties{};
  *propertiesChain = &powerProperties;
  propertiesChain = &(*propertiesChain)->nextInChain;

  GetHandle().GetInfo(&info);
  adapter_type_ = info.adapterType;
  backend_type_ = info.backendType;

  // TODO(crbug.com/359418629): Report xr compatibility in GetInfo()
  is_xr_compatible_ = options->xrCompatible();

  vendor_ = String::FromUtf8(info.vendor);
  architecture_ = String::FromUtf8(info.architecture);
  if (info.deviceID <= 0xffff) {
    device_ = Format("0x{:04x}", info.deviceID);
  } else {
    device_ = Format("0x{:08x}", info.deviceID);
  }
  description_ = String::FromUtf8(info.device);
  driver_ = String::FromUtf8(info.description);
  if (supportsPropertiesD3D) {
    d3d_shader_model_ = d3dProperties.shaderModel;
  }
  if (supportsPropertiesVk) {
    vk_driver_version_ = vkProperties.driverVersion;
  }
  subgroup_min_size_ = info.subgroupMinSize;
  subgroup_max_size_ = info.subgroupMaxSize;
  power_preference_ = powerProperties.powerPreference;

  // MZ: 如果配置了 WebGL 指纹，用相同的 GPU 信息替换 WebGPU。
  // 厂商一旦换掉，架构、描述、子组大小都必须跟着换：推断不出来就置空，
  // 绝不能留着真实显卡的值（例如伪装成 NVIDIA 却报 Intel 核显的 xe-lpg）。
  std::string configured_vendor = mz::fingerprint::BrowserFingerprint::GetConfiguredWebGLVendor();
  std::string configured_renderer = mz::fingerprint::BrowserFingerprint::GetConfiguredWebGLRenderer();

  if (!configured_vendor.empty() && !configured_renderer.empty()) {
    ParsedGPUInfo parsed = ParseWebGLRenderer(configured_vendor, configured_renderer);

    if (!parsed.vendor.empty()) {
      vendor_ = String::FromUtf8(parsed.vendor);
      architecture_ = String::FromUtf8(parsed.architecture);
      description_ = String::FromUtf8(parsed.description);
      // 驱动版本、设备 ID 只在开发者模式下暴露，真实值同样不能留
      driver_ = String();
      device_ = String();
      uint32_t min_size = 0;
      uint32_t max_size = 0;
      if (InferSubgroupSizes(parsed.vendor, parsed.architecture, &min_size, &max_size)) {
        subgroup_min_size_ = min_size;
        subgroup_max_size_ = max_size;
      }
    }
  }

  features_ = MakeFeatureNameSet(GetHandle());

  GPUSupportedLimits::ComboLimits limits;
  GetHandle().GetLimits(limits.GetLinked());
  limits_ = MakeGarbageCollected<GPUSupportedLimits>(limits);

  info_ = CreateAdapterInfoForAdapter();
}

GPUAdapterInfo* GPUAdapter::CreateAdapterInfoForAdapter() {
  bool is_fallback_adapter = adapter_type_ == wgpu::AdapterType::CPU;

  GPUAdapterInfo* info;
  if (RuntimeEnabledFeatures::WebGPUDeveloperFeaturesEnabled()) {
    // If WebGPU developer features have been enabled then provide all available
    // adapter info values.
    info = MakeGarbageCollected<GPUAdapterInfo>(
        vendor_, architecture_, subgroup_min_size_, subgroup_max_size_,
        is_fallback_adapter, device_, description_, driver_,
        FromDawnEnum(backend_type_), FromDawnEnum(adapter_type_),
        d3d_shader_model_, vk_driver_version_, FromDawnEnum(power_preference_));

    // SAFETY: Required from caller
    const auto memory_heaps_span =
        UNSAFE_BUFFERS(base::span<const wgpu::MemoryHeapInfo>(
            memory_heaps_.heapInfo, memory_heaps_.heapCount));
    for (const auto& m : memory_heaps_span) {
      info->AppendMemoryHeapInfo(MakeGarbageCollected<GPUMemoryHeapInfo>(m));
    }
  } else {
    info = MakeGarbageCollected<GPUAdapterInfo>(
        vendor_, architecture_, subgroup_min_size_, subgroup_max_size_,
        is_fallback_adapter);
  }

  // SAFETY: Required from caller
  const auto subgroup_matrix_configs_span =
      UNSAFE_BUFFERS(base::span<const wgpu::SubgroupMatrixConfig>(
          subgroup_matrix_configs_.configs,
          subgroup_matrix_configs_.configCount));
  for (const auto& c : subgroup_matrix_configs_span) {
    info->AppendSubgroupMatrixConfig(
        MakeGarbageCollected<GPUSubgroupMatrixConfig>(c));
  }

  return info;
}

void GPUAdapter::AddConsoleWarning(ExecutionContext* execution_context,
                                   const char* message) {
  if (execution_context && allowed_console_warnings_remaining_ > 0) {
    auto* console_message = MakeGarbageCollected<ConsoleMessage>(
        mojom::blink::ConsoleMessageSource::kRendering,
        mojom::blink::ConsoleMessageLevel::kWarning,
        StringFromASCIIAndUTF8(message));
    execution_context->AddConsoleMessage(console_message);

    allowed_console_warnings_remaining_--;
    if (allowed_console_warnings_remaining_ == 0) {
      auto* final_message = MakeGarbageCollected<ConsoleMessage>(
          mojom::blink::ConsoleMessageSource::kRendering,
          mojom::blink::ConsoleMessageLevel::kWarning,
          "WebGPU: too many warnings, no more warnings will be reported to the "
          "console for this GPUAdapter.");
      execution_context->AddConsoleMessage(final_message);
    }
  }
}

GPUSupportedFeatures* GPUAdapter::features() const {
  return features_.Get();
}

GPUAdapterInfo* GPUAdapter::info() const {
  return info_.Get();
}

wgpu::BackendType GPUAdapter::backendType() const {
  return backend_type_;
}

void GPUAdapter::OnRequestDeviceCallback(
    GPUDevice* device,
    const GPUDeviceDescriptor* descriptor,
    ScriptPromiseResolver<GPUDevice>* resolver,
    wgpu::RequestDeviceStatus status,
    wgpu::Device dawn_device,
    wgpu::StringView error_message) {
  switch (status) {
    case wgpu::RequestDeviceStatus::Success: {
      DCHECK(dawn_device);

      device->Initialize(dawn_device, descriptor, /*lost_info=*/nullptr);
      resolver->Resolve(device);

      ukm::builders::ClientRenderingAPI(
          device->GetExecutionContext()->UkmSourceID())
          .SetGPUDevice(static_cast<int>(true))
          .Record(device->GetExecutionContext()->UkmRecorder());
      break;
    }

    case wgpu::RequestDeviceStatus::Error:
    case wgpu::RequestDeviceStatus::CallbackCancelled:
      if (dawn_device) {
        // A device provided with an error is already a lost device on the Dawn
        // side, reflect that by resolving the lost property immediately.
        device->Initialize(dawn_device, descriptor,
                           MakeGarbageCollected<GPUDeviceLostInfo>(
                               wgpu::DeviceLostReason::Unknown,
                               StringFromASCIIAndUTF8(error_message)));

        // Resolve with the lost device.
        resolver->Resolve(device);
      } else {
        // If a device is not returned, that means that an error occurred while
        // validating features or limits, and as a result the promise should be
        // rejected with an OperationError.
        resolver->RejectWithDOMException(DOMExceptionCode::kOperationError,
                                         StringFromASCIIAndUTF8(error_message));
      }
      break;
  }
}

ScriptPromise<GPUDevice> GPUAdapter::requestDevice(
    ScriptState* script_state,
    GPUDeviceDescriptor* descriptor) {
  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver<GPUDevice>>(
      script_state, ExceptionContext(v8::ExceptionContext::kOperation,
                                     "GPUAdapter", "requestDevice"));
  auto promise = resolver->Promise();

  wgpu::DeviceDescriptor dawn_desc = {};

  wgpu::DawnConsumeAdapterDescriptor consume_adapter_desc;
  consume_adapter_desc.consumeAdapter = true;
  dawn_desc.nextInChain = &consume_adapter_desc;

  GPUSupportedLimits::ComboLimits required_limits;
  if (descriptor->hasRequiredLimits()) {
    dawn_desc.requiredLimits = required_limits.GetLinked();
    if (!GPUSupportedLimits::Populate(&required_limits,
                                      descriptor->requiredLimits(), resolver)) {
      return promise;
    }
  }

  // Use a set to prevent duplicate features.
  HashSet<wgpu::FeatureName> required_features_set;
  // The ShaderModuleCompilationOptions feature is required only if the adapter
  // has the ShaderModuleCompilationOptions feature and the user has enabled the
  // WebGPUDeveloperFeatures flag. It is needed to control
  // strict math during shader module compilation.
  if (RuntimeEnabledFeatures::WebGPUDeveloperFeaturesEnabled() &&
      GetHandle().HasFeature(
          wgpu::FeatureName::ShaderModuleCompilationOptions)) {
    required_features_set.insert(
        wgpu::FeatureName::ShaderModuleCompilationOptions);
  }
  if (descriptor->hasRequiredFeatures()) {
    for (const V8GPUFeatureName& f : descriptor->requiredFeatures()) {
      // If the feature is not a valid feature reject with a type error.
      if (!features_->Has(f.AsEnum())) {
        resolver->RejectWithTypeError(
            StrCat({"Unsupported feature: ", f.AsStringView()}));
        return promise;
      }
      required_features_set.insert(AsDawnEnum(f));
    }
  }

  Vector<wgpu::FeatureName> required_features(required_features_set);
  dawn_desc.requiredFeatures = required_features.data();
  dawn_desc.requiredFeatureCount = required_features.size();

  std::string label = descriptor->label().Utf8();
  if (!label.empty()) {
    dawn_desc.label = label.c_str();
  }

  std::string queueLabel = descriptor->defaultQueue()->label().Utf8();
  if (!queueLabel.empty()) {
    dawn_desc.defaultQueue.label = queueLabel.c_str();
  }

  // Create a GPUDevice without the handle, so that we can set up its callbacks
  // in the wgpu::DeviceDescriptor.
  ExecutionContext* execution_context = ExecutionContext::From(script_state);
  auto* device = MakeGarbageCollected<GPUDevice>(
      execution_context, GetDawnControlClient(), this, descriptor->label());
  device->SetDescriptorCallbacks(dawn_desc);

  auto* callback = MakeWGPUOnceCallback(resolver->WrapCallbackInScriptScope(
      BindOnce(&GPUAdapter::OnRequestDeviceCallback, WrapPersistent(this),
               WrapPersistent(device), WrapPersistent(descriptor))));
  GetHandle().RequestDevice(&dawn_desc, wgpu::CallbackMode::AllowProcessEvents,
                            callback->UnboundCallback(),
                            callback->AsUserdata());
  EnsureFlush(ToEventLoop(script_state));

  return promise;
}

void GPUAdapter::Trace(Visitor* visitor) const {
  visitor->Trace(gpu_);
  visitor->Trace(features_);
  visitor->Trace(limits_);
  visitor->Trace(info_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
