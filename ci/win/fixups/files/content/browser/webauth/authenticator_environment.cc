// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/webauth/authenticator_environment.h"

#include <algorithm>
#include <utility>

#include "base/no_destructor.h"
#include "base/not_fatal_until.h"
#include "content/browser/webauth/virtual_authenticator.h"
#include "content/browser/webauth/virtual_discovery.h"
#include "content/browser/webauth/virtual_fido_discovery_factory.h"
#include "content/public/browser/scoped_authenticator_environment_for_testing.h"
#include "device/fido/fido_discovery_factory.h"

// MZ: 每环境独立通行密钥
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "content/browser/webauth/virtual_authenticator_manager_impl.h"
#include "content/public/browser/mz_passkey.h"
#include "crypto/hash.h"
#include "device/fido/public/fido_constants.h"
#include "device/fido/public/public_key_credential_rp_entity.h"
#include "device/fido/public/public_key_credential_user_entity.h"
#include "device/fido/virtual_fido_device.h"

namespace content {

// ===== MZ: 每环境独立通行密钥（软件平台验证器） =====
namespace {

// Windows Hello 的 AAGUID：08987058-cadc-4b81-b6e1-30de50dcbe96
constexpr std::array<uint8_t, device::kAaguidLength> kMzDefaultAaguid = {
    0x08, 0x98, 0x70, 0x58, 0xca, 0xdc, 0x4b, 0x81,
    0xb6, 0xe1, 0x30, 0xde, 0x50, 0xdc, 0xbe, 0x96};

int64_t MzNowUnixMs() {
  return (base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds();
}

// 持有全局唯一的虚拟验证器管理器和其中的一个平台验证器，
// 观察凭据变化并转成 mz_passkey::Credential 交给回调（MZ EventBus）。
class MzPasskeyAuthenticatorHost : public VirtualAuthenticator::Observer {
 public:
  MzPasskeyAuthenticatorHost() = default;
  MzPasskeyAuthenticatorHost(const MzPasskeyAuthenticatorHost&) = delete;
  MzPasskeyAuthenticatorHost& operator=(const MzPasskeyAuthenticatorHost&) =
      delete;
  ~MzPasskeyAuthenticatorHost() override {
    if (authenticator_) {
      authenticator_->RemoveObserver(this);
      authenticator_ = nullptr;
    }
  }

  VirtualAuthenticatorManagerImpl* manager() { return manager_.get(); }

  void Enable(std::vector<mz_passkey::Credential> credentials,
              const mz_passkey::Options& options,
              mz_passkey::ChangeCallback on_change) {
    on_change_ = std::move(on_change);
    if (!manager_) {
      manager_ = std::make_unique<VirtualAuthenticatorManagerImpl>();
      VirtualAuthenticator::Options authenticator_options;
      authenticator_options.protocol = device::ProtocolVersion::kCtap2;
      authenticator_options.ctap2_version = device::Ctap2Version::kCtap2_0;
      authenticator_options.transport =
          device::FidoTransportProtocol::kInternal;
      authenticator_options.attachment =
          device::AuthenticatorAttachment::kPlatform;
      authenticator_options.has_resident_key = true;
      authenticator_options.has_user_verification = true;
      authenticator_options.is_user_present = true;
      authenticator_ = manager_->AddAuthenticatorAndReturnNonOwningPointer(
          authenticator_options);
      CHECK(authenticator_);
      authenticator_->AddObserver(this);
    }
    manager_->enable_ui(options.show_ui);
    authenticator_->SetMzPlatformAuthenticator(
        options.aaguid ? options.aaguid
                       : std::make_optional(kMzDefaultAaguid));
    // 内置用户验证和用户在场一律自动通过（不弹 Windows Hello）
    authenticator_->set_user_verified(true);
    authenticator_->SetUserPresence(true);

    // 重新加载：直接清空（不通知应用），再注入应用下发的凭据
    authenticator_->registrations().clear();
    created_at_.clear();
    size_t injected = 0;
    for (mz_passkey::Credential& credential : credentials) {
      if (credential.credential_id.empty() || credential.rp_id.empty()) {
        continue;
      }
      std::optional<std::unique_ptr<device::VirtualFidoDevice::PrivateKey>>
          private_key = device::VirtualFidoDevice::PrivateKey::FromPKCS8(
              credential.private_key_pkcs8);
      if (!private_key) {
        LOG(WARNING) << "[MZ][Passkey] 跳过私钥无法解析的凭据, rp="
                     << credential.rp_id;
        continue;
      }
      device::VirtualFidoDevice::RegistrationData registration(
          std::move(*private_key), crypto::hash::Sha256(credential.rp_id),
          credential.sign_count);
      registration.is_resident = credential.discoverable;
      registration.rp = device::PublicKeyCredentialRpEntity(credential.rp_id);
      registration.user = device::PublicKeyCredentialUserEntity(
          credential.user_handle,
          credential.user_name.empty()
              ? std::nullopt
              : std::make_optional(credential.user_name),
          credential.user_display_name.empty()
              ? std::nullopt
              : std::make_optional(credential.user_display_name));
      created_at_[credential.credential_id] = credential.created_at_ms > 0
                                                  ? credential.created_at_ms
                                                  : MzNowUnixMs();
      if (authenticator_->registrations()
              .emplace(credential.credential_id, std::move(registration))
              .second) {
        ++injected;
      }
    }
    LOG(INFO) << "[MZ][Passkey] 软件平台验证器已启用, 注入凭据 " << injected
              << " 条, ui=" << options.show_ui;
  }

  // VirtualAuthenticator::Observer:
  void OnCredentialCreated(
      VirtualAuthenticator* authenticator,
      const device::VirtualFidoDevice::Credential& credential) override {
    created_at_[std::vector<uint8_t>(credential.first.begin(),
                                     credential.first.end())] = MzNowUnixMs();
    Notify(mz_passkey::Change::kCreated, credential);
  }

  void OnCredentialDeleted(VirtualAuthenticator* authenticator,
                           base::span<const uint8_t> credential_id) override {
    mz_passkey::Credential deleted;
    deleted.credential_id.assign(credential_id.begin(), credential_id.end());
    created_at_.erase(deleted.credential_id);
    if (on_change_) {
      on_change_.Run(mz_passkey::Change::kDeleted, deleted);
    }
  }

  void OnCredentialUpdated(
      VirtualAuthenticator* authenticator,
      const device::VirtualFidoDevice::Credential& credential) override {
    Notify(mz_passkey::Change::kUpdated, credential);
  }

  // 每次断言后签名计数 +1，需要持久化
  void OnAssertion(
      VirtualAuthenticator* authenticator,
      const device::VirtualFidoDevice::Credential& credential) override {
    Notify(mz_passkey::Change::kUpdated, credential);
  }

  void OnAuthenticatorWillBeDestroyed(
      VirtualAuthenticator* authenticator) override {
    if (authenticator_ == authenticator) {
      authenticator_->RemoveObserver(this);
      authenticator_ = nullptr;
    }
  }

 private:
  void Notify(mz_passkey::Change change,
              const device::VirtualFidoDevice::Credential& credential) {
    if (!on_change_ || !credential.second) {
      return;
    }
    const device::VirtualFidoDevice::RegistrationData& registration =
        *credential.second;
    mz_passkey::Credential out;
    out.credential_id.assign(credential.first.begin(), credential.first.end());
    if (registration.rp) {
      out.rp_id = registration.rp->id;
    }
    if (registration.user) {
      out.user_handle = registration.user->id;
      out.user_name = registration.user->name.value_or(std::string());
      out.user_display_name =
          registration.user->display_name.value_or(std::string());
    }
    if (registration.private_key) {
      out.private_key_pkcs8 = registration.private_key->GetPKCS8PrivateKey();
    }
    out.sign_count = registration.counter.value_or(0);
    out.discoverable = registration.is_resident;
    auto it = created_at_.find(out.credential_id);
    out.created_at_ms = it != created_at_.end() ? it->second : MzNowUnixMs();
    on_change_.Run(change, out);
  }

  std::unique_ptr<VirtualAuthenticatorManagerImpl> manager_;
  raw_ptr<VirtualAuthenticator> authenticator_ = nullptr;
  mz_passkey::ChangeCallback on_change_;
  std::map<std::vector<uint8_t>, int64_t> created_at_;
};

MzPasskeyAuthenticatorHost& GetMzPasskeyHost() {
  static base::NoDestructor<MzPasskeyAuthenticatorHost> host;
  return *host;
}

}  // namespace

bool MzIsPasskeyAuthenticatorManager(VirtualAuthenticatorManagerImpl* manager);

// 供 authenticator_common_impl.cc 判断当前 frame 用的是 MZ 软件平台验证器
// （而不是 DevTools / WebDriver 的测试环境），以保留正常的超时和 TLS 检查。
bool MzIsPasskeyAuthenticatorManager(VirtualAuthenticatorManagerImpl* manager) {
  return manager && manager == GetMzPasskeyHost().manager();
}

namespace mz_passkey {

Credential::Credential() = default;
Credential::Credential(const Credential&) = default;
Credential::Credential(Credential&&) = default;
Credential& Credential::operator=(const Credential&) = default;
Credential& Credential::operator=(Credential&&) = default;
Credential::~Credential() = default;

Options::Options() = default;
Options::Options(const Options&) = default;
Options& Options::operator=(const Options&) = default;
Options::~Options() = default;

void EnablePlatformAuthenticator(std::vector<Credential> credentials,
                                 const Options& options,
                                 ChangeCallback on_change) {
  GetMzPasskeyHost().Enable(std::move(credentials), options,
                            std::move(on_change));
}

bool IsPlatformAuthenticatorEnabled() {
  return GetMzPasskeyHost().manager() != nullptr;
}

}  // namespace mz_passkey
// ===== MZ 结束 =====

ScopedAuthenticatorEnvironmentForTesting::
    ScopedAuthenticatorEnvironmentForTesting(
        std::unique_ptr<device::FidoDiscoveryFactory> factory) {
  AuthenticatorEnvironment* impl = AuthenticatorEnvironment::GetInstance();
  CHECK(impl->MaybeGetDiscoveryFactoryTestOverride() == nullptr);
  impl->ReplaceDefaultDiscoveryFactoryForTesting(std::move(factory));
}

ScopedAuthenticatorEnvironmentForTesting::
    ~ScopedAuthenticatorEnvironmentForTesting() {
  AuthenticatorEnvironment* impl = AuthenticatorEnvironment::GetInstance();
  CHECK(impl->MaybeGetDiscoveryFactoryTestOverride() != nullptr);
  impl->ReplaceDefaultDiscoveryFactoryForTesting(nullptr);
}

// static
AuthenticatorEnvironment* AuthenticatorEnvironment::GetInstance() {
  static base::NoDestructor<AuthenticatorEnvironment> environment;
  return environment.get();
}

AuthenticatorEnvironment::AuthenticatorEnvironment() = default;

AuthenticatorEnvironment::~AuthenticatorEnvironment() = default;

void AuthenticatorEnvironment::Reset() {
  for (const auto& pair : virtual_authenticator_managers_) {
    pair.first->RemoveObserver(this);
  }
  virtual_authenticator_managers_.clear();

  replaced_discovery_factory_.reset();
}

void AuthenticatorEnvironment::EnableVirtualAuthenticatorFor(
    FrameTreeNode* node,
    bool enable_ui) {
  // Do not create a new virtual authenticator if there is one already defined
  // for the |node|.
  auto [it, inserted] = virtual_authenticator_managers_.try_emplace(node);
  if (!inserted) {
    return;
  }

  node->AddObserver(this);
  it->second = std::make_unique<VirtualAuthenticatorManagerImpl>();
  it->second->enable_ui(enable_ui);
}

void AuthenticatorEnvironment::DisableVirtualAuthenticatorFor(
    FrameTreeNode* node) {
  auto it = virtual_authenticator_managers_.find(node);
  if (it == virtual_authenticator_managers_.end()) {
    return;
  }

  node->RemoveObserver(this);
  virtual_authenticator_managers_.erase(it);
}

bool AuthenticatorEnvironment::IsVirtualAuthenticatorEnabledFor(
    FrameTreeNode* node) {
  return MaybeGetVirtualAuthenticatorManager(node) != nullptr;
}

VirtualAuthenticatorManagerImpl*
AuthenticatorEnvironment::MaybeGetVirtualAuthenticatorManager(
    FrameTreeNode* node) {
  for (; node; node = FrameTreeNode::From(node->parent())) {
    if (auto it = virtual_authenticator_managers_.find(node);
        it != virtual_authenticator_managers_.end()) {
      return it->second.get();
    }
  }
  // MZ: 没有 DevTools/WebDriver 的测试环境时，所有 frame 都使用本环境的软件平台验证器
  return GetMzPasskeyHost().manager();
}

bool AuthenticatorEnvironment::HasVirtualUserVerifyingPlatformAuthenticator(
    FrameTreeNode* node) {
  VirtualAuthenticatorManagerImpl* authenticator_manager =
      MaybeGetVirtualAuthenticatorManager(node);
  if (!authenticator_manager) {
    return false;
  }
  std::vector<VirtualAuthenticator*> authenticators =
      authenticator_manager->GetAuthenticators();
  return std::ranges::any_of(authenticators, [](VirtualAuthenticator* a) {
    return a->is_user_verifying_platform_authenticator();
  });
}

device::FidoDiscoveryFactory*
AuthenticatorEnvironment::MaybeGetDiscoveryFactoryTestOverride() {
  return replaced_discovery_factory_.get();
}

void AuthenticatorEnvironment::ReplaceDefaultDiscoveryFactoryForTesting(
    std::unique_ptr<device::FidoDiscoveryFactory> factory) {
  replaced_discovery_factory_ = std::move(factory);
}

void AuthenticatorEnvironment::OnFrameTreeNodeDestroyed(FrameTreeNode* node) {
  DisableVirtualAuthenticatorFor(node);
}

}  // namespace content
