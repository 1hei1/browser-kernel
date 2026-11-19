// Copyright 2024 Simprint. All rights reserved.
// EventBus 核心实现

#include "mz/eventbus/eventbus.h"

#include "mz/totp/totp_store.h"
// MZ 通行密钥：每环境软件平台验证器
#include "base/base64url.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/mz_passkey.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime_desktop.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/common/url_constants.h"
#include "components/password_manager/core/browser/import/csv_password.h"
#include "components/password_manager/core/browser/import/password_importer.h"
#include "components/password_manager/core/browser/ui/saved_passwords_presenter.h"
#include "chrome/browser/password_manager/factories/account_password_store_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/affiliations/affiliation_service_factory.h"
#include "chrome/browser/sessions/exit_type_service.h"
#include "components/prefs/pref_service.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_options.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "mz/config/fingerprint_config_storage.h"
#include "mz/fingerprint/fingerprint_config.h"
#include "ui/gfx/geometry/rect.h"

// 统一日志输出
#define MZ_LOG(msg) LOG(INFO) << "[MZ][EventBus] " << msg

namespace mz {
namespace eventbus {

namespace {
// 同步角色：0=关闭 1=主控 2=从控，由 Tauri 下发 SyncRole 消息设置；供后续视图层捕获/重放使用
std::atomic<int> g_sync_role{0};

constexpr int kOpenUrlsRetryCount = 50;
constexpr base::TimeDelta kOpenUrlsRetryDelay = base::Milliseconds(100);

// ===== MZ 通行密钥 =====
// 应用下发 / 内核上报的凭据都用 JSON，二进制字段为无填充的 base64url。

std::string MzPasskeyBase64Url(const std::vector<uint8_t>& data) {
  std::string out;
  base::Base64UrlEncode(data, base::Base64UrlEncodePolicy::OMIT_PADDING, &out);
  return out;
}

std::vector<uint8_t> MzPasskeyFromBase64Url(const std::string* value) {
  if (!value || value->empty()) {
    return {};
  }
  std::optional<std::vector<uint8_t>> decoded = base::Base64UrlDecode(
      *value, base::Base64UrlDecodePolicy::IGNORE_PADDING);
  return decoded ? std::move(*decoded) : std::vector<uint8_t>();
}

std::string MzPasskeyString(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

// AAGUID：32 位十六进制，可带 "-"
bool MzParsePasskeyAaguid(const std::string& input,
                          std::array<uint8_t, 16>* out) {
  std::string hex;
  for (char c : input) {
    if (c != '-') {
      hex.push_back(c);
    }
  }
  return hex.size() == 32 && base::HexStringToSpan(hex, *out);
}

std::vector<content::mz_passkey::Credential> MzParseLaunchPasskeys(
    const base::ListValue& list) {
  std::vector<content::mz_passkey::Credential> credentials;
  for (const base::Value& item : list) {
    if (!item.is_dict()) {
      continue;
    }
    const base::DictValue& dict = item.GetDict();
    content::mz_passkey::Credential credential;
    credential.credential_id =
        MzPasskeyFromBase64Url(dict.FindString("credential_id"));
    credential.rp_id = MzPasskeyString(dict, "rp_id");
    credential.user_handle =
        MzPasskeyFromBase64Url(dict.FindString("user_handle"));
    credential.user_name = MzPasskeyString(dict, "user_name");
    credential.user_display_name = MzPasskeyString(dict, "user_display_name");
    credential.private_key_pkcs8 =
        MzPasskeyFromBase64Url(dict.FindString("private_key"));
    const double sign_count = dict.FindDouble("sign_count").value_or(0);
    credential.sign_count =
        sign_count > 0 && sign_count < 4294967295.0
            ? static_cast<uint32_t>(sign_count)
            : 0u;
    credential.created_at_ms =
        static_cast<int64_t>(dict.FindDouble("created_at").value_or(0));
    credential.discoverable = dict.FindBool("discoverable").value_or(true);
    if (credential.credential_id.empty() || credential.rp_id.empty() ||
        credential.private_key_pkcs8.empty()) {
      continue;
    }
    credentials.push_back(std::move(credential));
  }
  return credentials;
}

// 内核上报凭据变化（UI 线程）。私钥只经本机命名管道发给 MZ 应用，由应用加密落盘。
void MzSendPasskeyChange(content::mz_passkey::Change change,
                         const content::mz_passkey::Credential& credential) {
  base::DictValue dict;
  const char* action = "updated";
  if (change == content::mz_passkey::Change::kCreated) {
    action = "created";
  } else if (change == content::mz_passkey::Change::kDeleted) {
    action = "deleted";
  }
  dict.Set("action", action);
  dict.Set("credential_id", MzPasskeyBase64Url(credential.credential_id));
  if (change != content::mz_passkey::Change::kDeleted) {
    dict.Set("rp_id", credential.rp_id);
    dict.Set("user_handle", MzPasskeyBase64Url(credential.user_handle));
    dict.Set("user_name", credential.user_name);
    dict.Set("user_display_name", credential.user_display_name);
    dict.Set("private_key", MzPasskeyBase64Url(credential.private_key_pkcs8));
    dict.Set("sign_count", static_cast<double>(credential.sign_count));
    dict.Set("created_at", static_cast<double>(credential.created_at_ms));
    dict.Set("discoverable", credential.discoverable);
  }
  std::optional<std::string> json = base::WriteJson(dict);
  if (!json) {
    return;
  }
  const bool sent = EventBus::GetInstance().SendEvent(
      Topic::kPasskeyChanged, std::vector<uint8_t>(json->begin(), json->end()));
  LOG(INFO) << "[MZ][Passkey] 凭据" << action << " rp=" << credential.rp_id
            << (sent ? " 已上报" : " 上报失败（EventBus 未连接）");
}

void MzEnablePasskeys(std::vector<content::mz_passkey::Credential> credentials,
                      content::mz_passkey::Options options) {
  content::mz_passkey::EnablePlatformAuthenticator(
      std::move(credentials), options,
      base::BindRepeating(&MzSendPasskeyChange));
}
// ===== MZ 通行密钥结束 =====

struct CookieSiteTarget {
  std::string site_input;
  std::string domain;
  std::string path;
  bool secure = false;
};

struct LaunchCookieEntry {
  GURL source_url;
  std::string name;
  std::string value;
  std::string domain;
  std::string path;
  bool secure = false;
  bool http_only = false;
  net::CookieSameSite same_site = net::CookieSameSite::LAX_MODE;
};

struct CookieInjectionState {
  CookieInjectionState(int pending_cookies,
                       std::string injection_source,
                       base::OnceClosure completion)
      : pending(pending_cookies),
        source(std::move(injection_source)),
        done(std::move(completion)) {}

  int pending;
  int success_count = 0;
  int failure_count = 0;
  std::string source;
  base::OnceClosure done;
};

Profile* GetPrimaryLoadedProfile() {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (!profile_manager) {
    return nullptr;
  }

  std::vector<Profile*> profiles = profile_manager->GetLoadedProfiles();
  if (profiles.empty()) {
    return nullptr;
  }

  return profiles[0];
}

Browser* FindAnyTabbedBrowserForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }

  std::vector<Browser*> browsers = chrome::FindAllTabbedBrowsersWithProfile(profile);
  return browsers.empty() ? nullptr : browsers[0];
}

bool BrowserAlreadyHasUrl(Browser* browser, const GURL& target_url) {
  if (!browser || !browser->tab_strip_model()) {
    return false;
  }

  TabStripModel* tab_strip = browser->tab_strip_model();
  for (int i = 0; i < tab_strip->count(); ++i) {
    content::WebContents* web_contents = tab_strip->GetWebContentsAt(i);
    if (!web_contents) {
      continue;
    }

    if (web_contents->GetVisibleURL() == target_url ||
        web_contents->GetLastCommittedURL() == target_url) {
      return true;
    }
  }

  return false;
}

void RunWhenPrimaryProfileReady(base::OnceCallback<void(Profile*)> callback,
                                int retries_remaining,
                                std::string source) {
  Profile* profile = GetPrimaryLoadedProfile();
  if (profile) {
    std::move(callback).Run(profile);
    return;
  }

  if (retries_remaining <= 0) {
    MZ_LOG(source << ": Profile 尚未就绪，放弃执行");
    return;
  }

  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RunWhenPrimaryProfileReady, std::move(callback),
                     retries_remaining - 1, std::move(source)),
      kOpenUrlsRetryDelay);
}

void OpenUrlsWhenBrowserReady(std::vector<GURL> urls,
                              int retries_remaining,
                              std::string source) {
  if (urls.empty()) {
    return;
  }

  Profile* profile = GetPrimaryLoadedProfile();
  Browser* browser = FindAnyTabbedBrowserForProfile(profile);
  if (!browser) {
    if (retries_remaining <= 0) {
      MZ_LOG(source << ": 浏览器窗口尚未就绪，放弃打开 " << urls.size() << " 个 URL");
      return;
    }

    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&OpenUrlsWhenBrowserReady, std::move(urls),
                       retries_remaining - 1, std::move(source)),
        kOpenUrlsRetryDelay);
    return;
  }

  // 第一个网址放到前台：后台标签页拿不到窗口位置和尺寸，页面读到的 outerWidth 等都是 0
  bool opened_foreground = false;
  for (const GURL& url : urls) {
    if (BrowserAlreadyHasUrl(browser, url)) {
      MZ_LOG(source << ": 标签页已存在，跳过 " << url.spec());
      continue;
    }

    MZ_LOG(source << ": 打开标签页 " << url.spec());
    chrome::AddTabAt(browser, url, -1, !opened_foreground);
    opened_foreground = true;
  }
}

std::vector<GURL> ParseUrlList(const base::ListValue* values,
                               std::string_view source) {
  std::vector<GURL> urls;
  if (!values) {
    return urls;
  }

  for (const auto& value : *values) {
    if (!value.is_string()) {
      continue;
    }

    GURL url(value.GetString());
    if (!url.is_valid()) {
      MZ_LOG(source << ": 无效的 URL: " << value.GetString());
      continue;
    }

    if (std::find(urls.begin(), urls.end(), url) != urls.end()) {
      continue;
    }

    urls.push_back(url);
  }

  return urls;
}

std::string NormalizeCookiePath(std::string_view path) {
  std::string trimmed(
      base::TrimWhitespaceASCII(path, base::TRIM_ALL));
  if (trimmed.empty() || trimmed == "/") {
    return "/";
  }

  if (trimmed.front() == '/') {
    return trimmed;
  }

  return "/" + trimmed;
}

std::optional<CookieSiteTarget> ParseCookieSite(std::string_view site,
                                                std::string_view source) {
  std::string site_input(
      base::TrimWhitespaceASCII(site, base::TRIM_ALL));
  if (site_input.empty()) {
    MZ_LOG(source << ": Cookie 目标网页/域名不能为空");
    return std::nullopt;
  }

  GURL parsed_url(site_input);
  if (parsed_url.is_valid() && parsed_url.has_host()) {
    return CookieSiteTarget{
        .site_input = site_input,
        .domain = std::string(parsed_url.host()),
        .path = NormalizeCookiePath(parsed_url.path()),
        .secure = parsed_url.SchemeIs("https"),
    };
  }

  if (site_input.find("://") == std::string::npos) {
    if (site_input.front() == '.' && site_input.find('/') == std::string::npos &&
        std::none_of(site_input.begin(), site_input.end(),
                     [](char ch) { return base::IsAsciiWhitespace(ch); })) {
      return CookieSiteTarget{
          .site_input = site_input,
          .domain = site_input,
          .path = "/",
          .secure = false,
      };
    }

    GURL candidate_url("https://" + site_input);
    if (candidate_url.is_valid() && candidate_url.has_host()) {
      return CookieSiteTarget{
          .site_input = site_input,
          .domain = std::string(candidate_url.host()),
          .path = NormalizeCookiePath(candidate_url.path()),
          .secure = false,
      };
    }
  }

  MZ_LOG(source << ": 无效的 Cookie 目标网页/域名: " << site_input);
  return std::nullopt;
}

GURL BuildCookieSourceUrl(const std::string& domain,
                          const std::string& path,
                          bool secure) {
  std::string host = domain;
  while (!host.empty() && host.front() == '.') {
    host.erase(host.begin());
  }

  if (host.empty()) {
    return GURL();
  }

  return GURL(std::string(secure ? "https://" : "http://") + host +
              NormalizeCookiePath(path));
}

bool IsCookieAttribute(std::string_view part) {
  std::string lower = base::ToLowerASCII(
      base::TrimWhitespaceASCII(part, base::TRIM_ALL));
  return lower == "secure" || lower == "httponly" || lower == "http-only" ||
         lower == "partitioned" || base::StartsWith(lower, "domain=") ||
         base::StartsWith(lower, "path=") ||
         base::StartsWith(lower, "expires=") ||
         base::StartsWith(lower, "max-age=") ||
         base::StartsWith(lower, "samesite=");
}

std::optional<std::pair<std::string, std::string>> ParseCookieNameValue(
    std::string_view part,
    std::string_view source) {
  std::string trimmed(
      base::TrimWhitespaceASCII(part, base::TRIM_ALL));
  size_t eq_pos = trimmed.find('=');
  if (eq_pos == std::string::npos) {
    MZ_LOG(source << ": 无效的 Cookie 格式: " << trimmed);
    return std::nullopt;
  }

  std::string name(base::TrimWhitespaceASCII(trimmed.substr(0, eq_pos),
                                             base::TRIM_ALL));
  std::string value(base::TrimWhitespaceASCII(trimmed.substr(eq_pos + 1),
                                              base::TRIM_ALL));
  if (name.empty()) {
    MZ_LOG(source << ": Cookie 名称不能为空: " << trimmed);
    return std::nullopt;
  }

  return std::make_pair(std::move(name), std::move(value));
}

net::CookieSameSite ParseCookieSameSite(std::string_view same_site) {
  std::string lower =
      base::ToLowerASCII(base::TrimWhitespaceASCII(same_site, base::TRIM_ALL));
  if (lower == "none") {
    return net::CookieSameSite::NO_RESTRICTION;
  }
  if (lower == "strict") {
    return net::CookieSameSite::STRICT_MODE;
  }
  return net::CookieSameSite::LAX_MODE;
}

std::optional<LaunchCookieEntry> BuildLaunchCookie(
    std::string name,
    std::string value,
    std::string domain,
    std::string path,
    bool secure,
    bool http_only,
    net::CookieSameSite same_site,
    std::string_view source) {
  GURL source_url = BuildCookieSourceUrl(domain, path, secure);
  if (!source_url.is_valid()) {
    MZ_LOG(source << ": 无法构造 Cookie source URL, domain=" << domain
                        << ", path=" << path);
    return std::nullopt;
  }

  return LaunchCookieEntry{
      .source_url = std::move(source_url),
      .name = std::move(name),
      .value = std::move(value),
      .domain = std::move(domain),
      .path = NormalizeCookiePath(path),
      .secure = secure,
      .http_only = http_only,
      .same_site = same_site,
  };
}

std::optional<LaunchCookieEntry> ParseCookieLineWithAttrs(
    const CookieSiteTarget& site_target,
    const std::string& line,
    std::string_view source) {
  std::vector<std::string> parts =
      base::SplitString(line, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty()) {
    return std::nullopt;
  }

  auto name_value = ParseCookieNameValue(parts.front(), source);
  if (!name_value) {
    return std::nullopt;
  }

  std::string domain = site_target.domain;
  std::string path = site_target.path;
  bool secure = site_target.secure;
  bool http_only = false;
  net::CookieSameSite same_site = net::CookieSameSite::LAX_MODE;

  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    std::string lower =
        base::ToLowerASCII(base::TrimWhitespaceASCII(part, base::TRIM_ALL));
    if (base::StartsWith(lower, "domain=")) {
      domain = base::TrimWhitespaceASCII(part.substr(7), base::TRIM_ALL);
    } else if (base::StartsWith(lower, "path=")) {
      path = NormalizeCookiePath(part.substr(5));
    } else if (lower == "secure") {
      secure = true;
    } else if (lower == "httponly" || lower == "http-only") {
      http_only = true;
    } else if (base::StartsWith(lower, "samesite=")) {
      same_site = ParseCookieSameSite(part.substr(9));
    }
  }

  return BuildLaunchCookie(std::move(name_value->first),
                           std::move(name_value->second), std::move(domain),
                           std::move(path), secure, http_only, same_site,
                           source);
}

std::vector<LaunchCookieEntry> ParseLaunchCookies(
    const base::ListValue* cookie_groups,
    std::string_view source) {
  std::vector<LaunchCookieEntry> cookies;
  if (!cookie_groups) {
    return cookies;
  }

  for (const auto& group_value : *cookie_groups) {
    if (!group_value.is_dict()) {
      continue;
    }

    const base::DictValue& group = group_value.GetDict();
    const std::string* site = group.FindString("site");
    const std::string* cookie_text = group.FindString("cookie_text");
    if (!site || !cookie_text) {
      MZ_LOG(source << ": Cookie 分组缺少 site 或 cookie_text");
      continue;
    }

    auto site_target = ParseCookieSite(*site, source);
    if (!site_target) {
      continue;
    }

    std::vector<std::string> lines = base::SplitString(
        *cookie_text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    for (const std::string& line : lines) {
      std::vector<std::string> parts = base::SplitString(
          line, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      if (parts.empty()) {
        continue;
      }

      bool has_attr_style = parts.size() > 1 &&
                            std::all_of(parts.begin() + 1, parts.end(),
                                        [](const std::string& part) {
                                          return IsCookieAttribute(part);
                                        });
      if (has_attr_style) {
        auto cookie = ParseCookieLineWithAttrs(*site_target, line, source);
        if (cookie) {
          cookies.push_back(std::move(*cookie));
        }
        continue;
      }

      for (const std::string& part : parts) {
        auto name_value = ParseCookieNameValue(part, source);
        if (!name_value) {
          continue;
        }

        auto cookie = BuildLaunchCookie(std::move(name_value->first),
                                        std::move(name_value->second),
                                        site_target->domain, site_target->path,
                                        site_target->secure, false,
                                        net::CookieSameSite::LAX_MODE, source);
        if (cookie) {
          cookies.push_back(std::move(*cookie));
        }
      }
    }
  }

  return cookies;
}

void CompleteCookieInjection(const std::shared_ptr<CookieInjectionState>& state) {
  if (--state->pending > 0) {
    return;
  }

  MZ_LOG(state->source << ": Cookie 注入完成, success="
                             << state->success_count
                             << ", failure=" << state->failure_count);
  if (!state->done.is_null()) {
    std::move(state->done).Run();
  }
}

void StartAccountImport(Profile* profile,
                        std::vector<password_manager::CSVPassword> passwords,
                        std::vector<GURL> urls) {
  if (!profile || passwords.empty()) {
    return;
  }

  auto* affiliation_service = AffiliationServiceFactory::GetForProfile(profile);
  auto profile_store = ProfilePasswordStoreFactory::GetForProfile(
      profile, ServiceAccessType::EXPLICIT_ACCESS);
  auto account_store = AccountPasswordStoreFactory::GetForProfile(
      profile, ServiceAccessType::EXPLICIT_ACCESS);

  auto presenter = std::make_unique<password_manager::SavedPasswordsPresenter>(
      affiliation_service, profile_store, account_store);
  presenter->Init();

  auto importer = std::make_unique<password_manager::PasswordImporter>(
      *presenter, false);

  MZ_LOG("LaunchConfig: 开始导入 " << passwords.size() << " 个密码");
  importer->Import(
      passwords, password_manager::PasswordForm::Store::kProfileStore,
      base::BindOnce(
          [](std::unique_ptr<password_manager::SavedPasswordsPresenter> presenter,
             std::unique_ptr<password_manager::PasswordImporter> importer,
             std::vector<GURL> urls,
             const password_manager::ImportResults& results) {
            MZ_LOG("LaunchConfig: 密码导入完成，状态="
                         << static_cast<int>(results.status));
            MZ_LOG("LaunchConfig: 导入数量=" << results.number_imported);

            if (results.status ==
                password_manager::ImportResults::Status::SUCCESS) {
              OpenUrlsWhenBrowserReady(std::move(urls), kOpenUrlsRetryCount,
                                       "LaunchConfig.accounts");
            }
          },
          std::move(presenter), std::move(importer), std::move(urls)));
}

void ContinueLaunchConfigActions(
    Profile* profile,
    std::vector<password_manager::CSVPassword> passwords,
    std::vector<GURL> account_urls,
    std::vector<GURL> launch_urls) {
  if (!passwords.empty()) {
    StartAccountImport(profile, std::move(passwords), std::move(account_urls));
  }

  if (!launch_urls.empty()) {
    OpenUrlsWhenBrowserReady(std::move(launch_urls), kOpenUrlsRetryCount,
                             "LaunchConfig.urls");
  }
}

void InjectLaunchCookies(Profile* profile,
                         std::vector<LaunchCookieEntry> cookies,
                         base::OnceClosure done,
                         std::string source) {
  if (!profile || cookies.empty()) {
    if (!done.is_null()) {
      std::move(done).Run();
    }
    return;
  }

  content::StoragePartition* storage_partition =
      profile->GetDefaultStoragePartition();
  if (!storage_partition) {
    MZ_LOG(source << ": StoragePartition 不可用");
    if (!done.is_null()) {
      std::move(done).Run();
    }
    return;
  }

  network::mojom::CookieManager* cookie_manager =
      storage_partition->GetCookieManagerForBrowserProcess();
  if (!cookie_manager) {
    MZ_LOG(source << ": CookieManager 不可用");
    if (!done.is_null()) {
      std::move(done).Run();
    }
    return;
  }

  MZ_LOG(source << ": 开始注入 " << cookies.size() << " 条 Cookie");
  auto state =
      std::make_shared<CookieInjectionState>(cookies.size(), std::move(source),
                                             std::move(done));

  for (auto& cookie : cookies) {
    auto canonical_cookie = net::CanonicalCookie::CreateSanitizedCookie(
        cookie.source_url, cookie.name, cookie.value, cookie.domain, cookie.path,
        base::Time::Now(), base::Time(), base::Time::Now(), cookie.secure,
        cookie.http_only, cookie.same_site, net::COOKIE_PRIORITY_DEFAULT,
        std::nullopt, /*status=*/nullptr);

    if (!canonical_cookie) {
      MZ_LOG(state->source << ": 无法创建 Cookie: " << cookie.name
                                 << " @ " << cookie.domain);
      ++state->failure_count;
      CompleteCookieInjection(state);
      continue;
    }

    net::CookieOptions options = net::CookieOptions::MakeAllInclusive();
    cookie_manager->SetCanonicalCookie(
        *canonical_cookie, cookie.source_url, options,
        base::BindOnce(
            [](const std::shared_ptr<CookieInjectionState>& state,
               std::string cookie_name,
               std::string cookie_domain,
               net::CookieAccessResult result) {
              if (result.status.IsInclude()) {
                ++state->success_count;
              } else {
                ++state->failure_count;
                MZ_LOG(state->source << ": Cookie 注入失败: "
                                           << cookie_name << " @ "
                                           << cookie_domain << ", status="
                                           << result.status.GetDebugString());
              }
              CompleteCookieInjection(state);
            },
            state, cookie.name, cookie.domain));
  }
}
}  // namespace

// static
int EventBus::GetSyncRole() {
  return g_sync_role.load(std::memory_order_relaxed);
}

void EventBus::SetSyncReplayCallback(SyncReplayCallback callback) {
  // 允许从 UI 线程注册（由 BrowserView::InitViews 调用），与消息接收的 pipe 线程不同
  sync_replay_callback_ = std::move(callback);
}

void EventBus::SetSyncPasteReplayCallback(SyncPasteReplayCallback callback) {
  sync_paste_replay_callback_ = std::move(callback);
}

// static
EventBus& EventBus::GetInstance() {
  static base::NoDestructor<EventBus> instance;
  return *instance;
}

EventBus::EventBus() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

EventBus::~EventBus() {
  Shutdown();
}

EventBus::AuthInfo::AuthInfo() = default;

EventBus::AuthInfo::~AuthInfo() = default;

void EventBus::Initialize(const std::string& env_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  MZ_LOG("Initialize called with env_id=" << env_id);

  if (state_ != EventBusState::kUninitialized) {
    MZ_LOG("Already initialized");
    return;
  }

  if (env_id.empty()) {
    MZ_LOG("No env_id provided, skipping");
    return;
  }

  env_id_ = env_id;
  MZ_LOG("Creating transport...");

  SetState(EventBusState::kConnecting);

  // 创建传输层
  transport_ = CreateTransport(env_id_, this);

  MZ_LOG("Calling Connect()...");

  // 连接
  transport_->Connect();

  MZ_LOG("Connect() returned");
}

void EventBus::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (state_ == EventBusState::kUninitialized) {
    return;
  }

  LOG(INFO) << "EventBus: Shutting down";

  // 发送断开消息
  if (transport_ && transport_->IsConnected()) {
    SendEvent(Topic::kDisconnect);
  }

  // 断开连接
  if (transport_) {
    transport_->Disconnect();
    transport_.reset();
  }

  // 清理
  handlers_.clear();
  owned_handlers_.clear();
  handshake_complete_ = false;

  SetState(EventBusState::kUninitialized);
}

bool EventBus::IsInitialized() const {
  return state_ != EventBusState::kUninitialized;
}

bool EventBus::IsConnected() const {
  return state_ == EventBusState::kConnected && handshake_complete_;
}

bool EventBus::SendRequest(Topic topic, std::vector<uint8_t> data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!transport_ || !transport_->IsConnected()) {
    LOG(WARNING) << "EventBus: Cannot send request - not connected";
    return false;
  }

  auto message = Message::Request(topic, std::move(data));

  return transport_->Send(message);
}

bool EventBus::SendEvent(Topic topic, std::vector<uint8_t> data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!transport_ || !transport_->IsConnected()) {
    LOG(WARNING) << "EventBus: Cannot send event - not connected";
    return false;
  }

  auto message = Message::Event(topic, std::move(data));

  return transport_->Send(message);
}

void EventBus::SetAuthStatusCallback(AuthStatusCallback callback) {
  auth_status_callback_ = std::move(callback);
}

void EventBus::RequestAuthInfo() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConnected()) {
    return;
  }

  SendRequest(Topic::kAuthRequest, {});
}

void EventBus::RequestAuthInfoWithTimeout(
    int timeout_ms,
    base::OnceClosure timeout_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // 启动超时计时器（无论是否已连接）
  auth_timeout_timer_ = std::make_unique<base::OneShotTimer>();
  auth_timeout_timer_->Start(
      FROM_HERE,
      base::Milliseconds(timeout_ms),
      std::move(timeout_callback));

  // 如果已连接，立即发送请求；否则标记为待处理，等待握手完成后发送
  if (IsConnected()) {
    SendRequest(Topic::kAuthRequest, {});
  } else {
    pending_auth_request_ = true;
  }
}

void EventBus::RegisterHandler(std::unique_ptr<MessageHandler> handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto* raw_handler = handler.get();
  owned_handlers_.push_back(std::move(handler));

  for (Topic topic : raw_handler->GetTopics()) {
    handlers_[topic].push_back(raw_handler);
    LOG(INFO) << "EventBus: Registered handler for topic "
              << static_cast<int>(topic);
  }
}

void EventBus::RegisterHandler(Topic topic, HandlerFunc func) {
  RegisterHandler(std::vector<Topic>{topic}, std::move(func));
}

void EventBus::RegisterHandler(std::vector<Topic> topics, HandlerFunc func) {
  RegisterHandler(
      std::make_unique<FunctionHandler>(std::move(topics), std::move(func)));
}

void EventBus::AddListener(EventBusListener* listener) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  listeners_.push_back(listener);
}

void EventBus::RemoveListener(EventBusListener* listener) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  listeners_.erase(
      std::remove(listeners_.begin(), listeners_.end(), listener),
      listeners_.end());
}

// TransportDelegate 实现

void EventBus::OnConnected() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  MZ_LOG("OnConnected called, starting handshake");

  SetState(EventBusState::kConnected);

  // 开始握手
  DoHandshake();
}

void EventBus::OnDisconnected() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "EventBus: Transport disconnected";

  handshake_complete_ = false;
  SetState(EventBusState::kDisconnected);
}

void EventBus::OnMessageReceived(Message message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);


  // 检查是否是握手响应
  if (!handshake_complete_ &&
      message.topic == Topic::kHandshake &&
      message.msg_type == MessageType::kResponse &&
      message.msg_id == handshake_request_id_) {
    HandleHandshakeResponse(message);
    return;
  }

  // 分发消息
  DispatchMessage(message);
}

void EventBus::OnError(const std::string& error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(ERROR) << "EventBus: Transport error - " << error;

  handshake_complete_ = false;
  SetState(EventBusState::kError);
}

// 内部方法

void EventBus::DoHandshake() {
  MZ_LOG("DoHandshake called");

  auto handshake_data = HandshakeData::Browser(env_id_);
  auto data = handshake_data.ToBytes();
  size_t data_size = data.size();

  auto message = Message::Request(Topic::kHandshake, std::move(data));
  handshake_request_id_ = message.msg_id;

  MZ_LOG("Sending handshake, msg_id=" << handshake_request_id_ << ", data_size=" << data_size);

  bool sent = transport_->Send(message);
  MZ_LOG("Handshake send result: " << (sent ? "success" : "failed"));
}

namespace {

// MZ: 打开 chrome://review（环境检测页），放在第一个标签并切过去。浏览器窗口还没创建时每 200ms
// 重试一次，最多 kReviewTabAttempts 次（约 15 秒）；已经有检测页就不再开
constexpr int kReviewTabAttempts = 75;

void OpenReviewTabWhenReady(int attempts_left) {
  ProfileManager* profile_manager =
      g_browser_process ? g_browser_process->profile_manager() : nullptr;
  std::vector<Profile*> profiles =
      profile_manager ? profile_manager->GetLoadedProfiles() : std::vector<Profile*>();
  Profile* profile = profiles.empty() ? nullptr : profiles[0];
  Browser* browser = profile ? chrome::FindTabbedBrowser(profile, false) : nullptr;
  if (!browser) {
    if (attempts_left > 0) {
      content::GetUIThreadTaskRunner({})->PostDelayedTask(
          FROM_HERE, base::BindOnce(&OpenReviewTabWhenReady, attempts_left - 1),
          base::Milliseconds(200));
    } else {
      LOG(WARNING) << "[MZ][EventBus] 没有等到浏览器窗口，未打开 chrome://review";
    }
    return;
  }

  for (Browser* candidate : *BrowserList::GetInstance()) {
    if (candidate->GetProfile() != profile) {
      continue;
    }
    TabStripModel* tab_strip = candidate->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* web_contents = tab_strip->GetWebContentsAt(i);
      if (!web_contents) {
        continue;
      }
      const GURL& url = web_contents->GetVisibleURL();
      if (url.SchemeIs(content::kChromeUIScheme) && url.host() == "review") {
        LOG(INFO) << "[MZ][EventBus] chrome://review 标签页已存在，跳过打开";
        return;
      }
    }
  }

  // 已经有启动网址在前台时不抢焦点，检测页只放在第一个位置
  content::WebContents* active = browser->tab_strip_model()->GetActiveWebContents();
  const bool web_page_active = active && active->GetVisibleURL().SchemeIsHTTPOrHTTPS();
  LOG(INFO) << "[MZ][EventBus] 打开 chrome://review 标签页";
  chrome::AddTabAt(browser, GURL("chrome://review/"), 0, !web_page_active);
}

}  // namespace

void EventBus::HandleHandshakeResponse(const Message& message) {
  if (message.error_code != 0) {
    LOG(ERROR) << "EventBus: Handshake failed, error_code=" << message.error_code;
    SetState(EventBusState::kError);
    return;
  }

  LOG(INFO) << "EventBus: Handshake successful";
  handshake_complete_ = true;

  // 立即将退出类型设置为 Clean，避免下次启动时提示恢复页面
  // 这样即使浏览器异常退出，退出类型也已经是 Clean 了
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (profile_manager) {
    std::vector<Profile*> profiles = profile_manager->GetLoadedProfiles();
    MZ_LOG("Setting clean exit type for " << profiles.size() << " profiles at startup");
    for (Profile* profile : profiles) {
      if (ExitTypeService* exit_type_service =
              ExitTypeService::GetInstanceForProfile(profile)) {
        exit_type_service->SetCurrentSessionExitType(ExitType::kClean);
        // 立即提交写入
        if (profile->GetPrefs()) {
          profile->GetPrefs()->CommitPendingWrite();
        }
      }
    }
  }

  // 注册 Disconnect handler，收到关闭请求时关闭浏览器
  RegisterHandler(Topic::kDisconnect, [](const Message& msg) -> std::optional<HandlerResponse> {
    MZ_LOG("Received Disconnect request from Tauri, closing browser");
    // 退出类型已经在启动时设置为 Clean，直接关闭即可
    chrome::CloseAllBrowsersAndQuit();
    return std::nullopt;
  });

  // 注册 WindowSetBounds handler，收到窗口布局请求时调整主窗口位置和大小
  RegisterHandler(Topic::kWindowSetBounds, [](const Message& msg) -> std::optional<HandlerResponse> {
    MZ_LOG("Received WindowSetBounds request from Tauri");

    // 期望数据格式：按小端序编码的 4 个 int32_t：x, y, width, height
    if (msg.data.size() < sizeof(int32_t) * 4) {
      LOG(WARNING) << "WindowSetBounds: invalid payload size=" << msg.data.size();
      return std::nullopt;
    }

    auto read_int32_le = [&msg](size_t index) -> int32_t {
      const size_t offset = index * sizeof(int32_t);
      // 已在调用处保证总长度 >= 4 * sizeof(int32_t)，这里再做一次防御性检查
      if (offset + sizeof(int32_t) > msg.data.size()) {
        return 0;
      }
      const uint8_t b0 = msg.data[offset];
      const uint8_t b1 = msg.data[offset + 1];
      const uint8_t b2 = msg.data[offset + 2];
      const uint8_t b3 = msg.data[offset + 3];
      uint32_t value = static_cast<uint32_t>(b0) |
                       (static_cast<uint32_t>(b1) << 8) |
                       (static_cast<uint32_t>(b2) << 16) |
                       (static_cast<uint32_t>(b3) << 24);
      return static_cast<int32_t>(value);
    };

    int32_t x = read_int32_le(0);
    int32_t y = read_int32_le(1);
    int32_t width = read_int32_le(2);
    int32_t height = read_int32_le(3);

    // 简单校验，避免传入非法尺寸
    if (width <= 0 || height <= 0) {
      LOG(WARNING) << "WindowSetBounds: invalid size, width=" << width
                   << ", height=" << height;
      return std::nullopt;
    }

    // 找到当前活动浏览器窗口
    Browser* browser = chrome::FindLastActive();
    if (!browser || !browser->GetWindow()) {
      LOG(WARNING) << "WindowSetBounds: no active browser window found";
      return std::nullopt;
    }

    gfx::Rect bounds(x, y, width, height);
    MZ_LOG("WindowSetBounds: applying bounds x=" << x
                                                       << ", y=" << y
                                                       << ", w=" << width
                                                       << ", h=" << height);
    browser->GetWindow()->SetBounds(bounds);

    return std::nullopt;
  });

  // 注册 SyncRole handler：设置本机为主控(1)/从控(2)/关闭(0)；捕获与重放由视图层实现
  RegisterHandler(Topic::kSyncRole, [](const Message& msg) -> std::optional<HandlerResponse> {
    if (!msg.data.empty()) {
      int role = static_cast<int>(msg.data[0]);
      if (role >= 0 && role <= 2) {
        g_sync_role.store(role, std::memory_order_relaxed);
        MZ_LOG("SyncRole set to " << role);
      }
    }
    return std::nullopt;
  });

  // 注册 SyncInputEvent handler：从控时调用已注册的 sync_replay_callback_，在视图层重放
  RegisterHandler(Topic::kSyncInputEvent, [this](const Message& msg) -> std::optional<HandlerResponse> {
    if (GetSyncRole() != 2) {
      VLOG(1) << "EventBus: SyncInputEvent ignored (role=" << GetSyncRole() << ", need 2=slave)";
      return std::nullopt;
    }
    if (!sync_replay_callback_) {
      LOG(WARNING) << "EventBus: SyncInputEvent received but sync_replay_callback_ is null";
      return std::nullopt;
    }
    sync_replay_callback_(msg.data);
    return std::nullopt;
  });

  // 注册 SyncPaste handler：从控时通过 sync_paste_replay_callback_ 插入文本
  RegisterHandler(Topic::kSyncPaste, [this](const Message& msg) -> std::optional<HandlerResponse> {
    if (GetSyncRole() != 2) return std::nullopt;
    if (!sync_paste_replay_callback_) return std::nullopt;
    std::string utf8(msg.data.begin(), msg.data.end());
    std::u16string text;
    if (!base::UTF8ToUTF16(utf8.data(), utf8.size(), &text)) return std::nullopt;
    sync_paste_replay_callback_(std::move(text));
    return std::nullopt;
  });

  // 注册 LaunchConfig handler：接收 Tauri 发送的启动配置（user_data_dir、proxy 等）
  RegisterHandler(Topic::kLaunchConfig, [](const Message& msg) -> std::optional<HandlerResponse> {
    if (msg.data.empty()) {
      MZ_LOG("LaunchConfig: 接收到空数据");
      return std::nullopt;
    }

    std::string json_str(msg.data.begin(), msg.data.end());
    auto value = base::JSONReader::Read(json_str, base::JSON_PARSE_RFC);

    if (!value || !value->is_dict()) {
      MZ_LOG("LaunchConfig: JSON 解析失败");
      return std::nullopt;
    }

    const base::DictValue& config = value->GetDict();
    MZ_LOG("LaunchConfig: 成功接收配置，包含 " << config.size() << " 个字段");

    // 打印所有配置字段
    for (auto item : config) {
      const std::string& key = item.first;
      const base::Value& val = item.second;
      std::ostringstream val_str;
      if (val.is_string()) {
        val_str << "\"" << val.GetString() << "\"";
      } else if (val.is_bool()) {
        val_str << (val.GetBool() ? "true" : "false");
      } else if (val.is_int()) {
        val_str << val.GetInt();
      } else if (val.is_dict()) {
        val_str << "{...}";
      } else if (val.is_list()) {
        val_str << "[...]";
      } else {
        val_str << "(其他类型)";
      }
      MZ_LOG("  - " << key << ": " << val_str.str());
    }

    std::vector<GURL> launch_urls =
        ParseUrlList(config.FindList("urls"), "LaunchConfig.urls");
    std::vector<LaunchCookieEntry> launch_cookies =
        ParseLaunchCookies(config.FindList("cookies"), "LaunchConfig.cookies");
    if (!launch_cookies.empty()) {
      MZ_LOG("LaunchConfig: 发现 " << launch_cookies.size()
                                         << " 条待注入 Cookie");
    }

    // 处理账号导入
    std::vector<password_manager::CSVPassword> csv_passwords;
    std::vector<GURL> account_urls;
    // 2FA：账号的 totp_secret 只保存在内存里，供新标签页显示验证码
    std::vector<mz::totp::Account> totp_accounts;
    const base::ListValue* accounts = config.FindList("accounts");
    if (accounts && !accounts->empty()) {
      MZ_LOG("LaunchConfig: 发现 " << accounts->size() << " 个账号，准备导入");

      for (const auto& account_value : *accounts) {
        if (!account_value.is_dict()) {
          continue;
        }

        const base::DictValue& account = account_value.GetDict();
        const std::string* url = account.FindString("url");
        const std::string* username = account.FindString("username");
        const std::string* password = account.FindString("password");

        if (!url || !username || !password) {
          MZ_LOG("LaunchConfig: 账号数据不完整，跳过");
          continue;
        }

        GURL gurl(*url);
        if (!gurl.is_valid()) {
          MZ_LOG("LaunchConfig.accounts: 账号 URL 无效，跳过");
          continue;
        }

        csv_passwords.emplace_back(
            gurl,
            *username,
            *password,
            "",  // note
            password_manager::CSVPassword::Status::kOK);

        account_urls.push_back(gurl);

        const std::string* totp_secret = account.FindString("totp_secret");
        if (totp_secret && !totp_secret->empty()) {
          totp_accounts.push_back({*url, *username, *totp_secret});
        }
      }
    }
    if (!totp_accounts.empty()) {
      MZ_LOG("LaunchConfig: " << totp_accounts.size() << " 个账号带 2FA 密钥");
    }
    mz::totp::SetAccounts(std::move(totp_accounts));

    // 通行密钥：配置里带 passkeys（可为空数组）时启用每环境软件平台验证器，
    // 替代 Windows Hello；旧版应用不发这个字段，行为不变
    if (const base::ListValue* passkeys = config.FindList("passkeys")) {
      std::vector<content::mz_passkey::Credential> passkey_credentials =
          MzParseLaunchPasskeys(*passkeys);
      content::mz_passkey::Options passkey_options;
      passkey_options.show_ui =
          !config.FindBool("passkey_silent").value_or(false);
      if (const std::string* aaguid = config.FindString("passkey_aaguid")) {
        std::array<uint8_t, 16> aaguid_bytes;
        if (MzParsePasskeyAaguid(*aaguid, &aaguid_bytes)) {
          passkey_options.aaguid = aaguid_bytes;
        }
      }
      MZ_LOG("LaunchConfig: 通行密钥 " << passkey_credentials.size() << " 条");
      if (content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
        MzEnablePasskeys(std::move(passkey_credentials), passkey_options);
      } else {
        content::GetUIThreadTaskRunner({})->PostTask(
            FROM_HERE, base::BindOnce(&MzEnablePasskeys,
                                      std::move(passkey_credentials),
                                      passkey_options));
      }
    }

    if (!launch_cookies.empty() || !csv_passwords.empty()) {
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE,
          base::BindOnce(
              &RunWhenPrimaryProfileReady,
              base::BindOnce(
                  [](std::vector<LaunchCookieEntry> cookies,
                     std::vector<password_manager::CSVPassword> passwords,
                     std::vector<GURL> account_urls,
                     std::vector<GURL> launch_urls,
                     Profile* profile) {
                    InjectLaunchCookies(
                        profile, std::move(cookies),
                        base::BindOnce(&ContinueLaunchConfigActions, profile,
                                       std::move(passwords),
                                       std::move(account_urls),
                                       std::move(launch_urls)),
                        "LaunchConfig.cookies");
                  },
                  std::move(launch_cookies), std::move(csv_passwords),
                  std::move(account_urls), std::move(launch_urls)),
              kOpenUrlsRetryCount, "LaunchConfig.cookies"));
    } else if (!launch_urls.empty()) {
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE,
          base::BindOnce(&OpenUrlsWhenBrowserReady, std::move(launch_urls),
                         kOpenUrlsRetryCount, "LaunchConfig.urls"));
    }

    // 注意：这些配置在浏览器启动后接收，大部分配置（如 user_data_dir、proxy）
    // 需要在启动时通过命令行参数设置，这里接收主要用于验证配置是否正确传递
    // 未来可能用于动态配置更新（如扩展加载）

    return std::nullopt;
  });

  // 注册 FingerprintApply handler：接收 Tauri 发送的指纹配置
  RegisterHandler(Topic::kFingerprintApply, [](const Message& msg) -> std::optional<HandlerResponse> {
    if (msg.data.empty()) {
      MZ_LOG("FingerprintApply: 接收到空数据");
      return HandlerResponse::Error(ErrorCode::kInvalidMessage);
    }

    std::string json_str(msg.data.begin(), msg.data.end());

    // 使用指纹配置管理器解析并应用配置
    auto* config_manager = mz::fingerprint::FingerprintConfigManager::GetInstance();
    if (config_manager->ApplyConfigFromJson(json_str)) {
      MZ_LOG("FingerprintApply: 指纹配置已成功加载到管理器");
      // 严格校验：配置残缺或值非法时拒绝（回应错误并说明原因），而不是用默认指纹继续运行
      std::string validation_error;
      if (!mz::fingerprint::ValidateImportedConfig(&validation_error)) {
        LOG(ERROR) << "[MZ][EventBus] FingerprintApply 被拒绝: " << validation_error;
        return HandlerResponse::Error(
            ErrorCode::kInvalidMessage,
            std::vector<uint8_t>(validation_error.begin(), validation_error.end()));
      }
      // 通知启动流程：指纹配置已应用（启动会等待它，见 WaitForInitialLaunchConfig）
      mz::config::MarkFingerprintApplied();

      // MZ: 配置加载成功后，更新所有 Profile 的 Accept-Language
      LOG(INFO) << "[MZ Language] Config loaded, updating Accept-Language for all profiles";

      ProfileManager* profile_manager = g_browser_process->profile_manager();
      if (profile_manager) {
        std::vector<Profile*> profiles = profile_manager->GetLoadedProfiles();
        LOG(INFO) << "[MZ Language] Found " << profiles.size() << " loaded profiles";

        for (Profile* profile : profiles) {
          ProfileNetworkContextService* service =
              ProfileNetworkContextServiceFactory::GetForContext(profile);
          if (service) {
            LOG(INFO) << "[MZ Language] Calling UpdateAcceptLanguage for profile";
            service->UpdateAcceptLanguage();
          } else {
            LOG(WARNING) << "[MZ Language] ProfileNetworkContextService not found for profile";
          }
        }

        // MZ: 配置加载成功后打开 chrome://review（环境检测页）。配置往往比第一个浏览器窗口先到，
        // 所以窗口还没出来时会等一会儿再开
        OpenReviewTabWhenReady(kReviewTabAttempts);
      } else {
        LOG(WARNING) << "[MZ Language] ProfileManager not found";
      }
    } else {
      MZ_LOG("FingerprintApply: 指纹配置加载失败");
      // 回应错误：应用据此拒绝启动，而不是让浏览器带着默认指纹运行
      return HandlerResponse::Error(ErrorCode::kInvalidMessage);
    }

    // 回应成功并回传实际采用的关键值：应用等待这个确认、并与自己发送的配置比对后，
    // 才把环境标记为已启动
    const std::string effective = mz::fingerprint::BuildEffectiveConfigSummary();
    return HandlerResponse::Success(
        std::vector<uint8_t>(effective.begin(), effective.end()));
  });

  // 注册 AuthResponse handler：接收 Tauri 发送的认证信息
  RegisterHandler(Topic::kAuthResponse, [this](const Message& msg) -> std::optional<HandlerResponse> {
    if (auth_timeout_timer_ && auth_timeout_timer_->IsRunning()) {
      auth_timeout_timer_->Stop();
    }

    if (msg.data.empty()) {
      return std::nullopt;
    }

    std::string json_str(msg.data.begin(), msg.data.end());
    auto value = base::JSONReader::Read(json_str, base::JSON_PARSE_RFC);

    if (!value || !value->is_dict()) {
      return std::nullopt;
    }

    const base::DictValue& auth_data = value->GetDict();

    std::optional<bool> is_authenticated = auth_data.FindBool("is_authenticated");
    const std::string* access_token = auth_data.FindString("access_token");
    const base::DictValue* user_info = auth_data.FindDict("user_info");

    cached_auth_info_.is_authenticated = is_authenticated.value_or(false);
    cached_auth_info_.access_token = access_token ? *access_token : "";

    if (user_info) {
      const std::string* user_id = user_info->FindString("user_id");
      const std::string* username = user_info->FindString("username");
      cached_auth_info_.user_id = user_id ? *user_id : "";
      cached_auth_info_.username = username ? *username : "";
    } else {
      cached_auth_info_.user_id.clear();
      cached_auth_info_.username.clear();
    }

    if (auth_status_callback_) {
      auth_status_callback_(cached_auth_info_);
    }

    return std::nullopt;
  });

  // 如果有待处理的认证请求，现在发送
  if (pending_auth_request_) {
    pending_auth_request_ = false;
    SendRequest(Topic::kAuthRequest, {});
  }

  NotifyHandshakeComplete();
}

void EventBus::DispatchMessage(const Message& message) {
  auto it = handlers_.find(message.topic);
  if (it == handlers_.end()) {
    LOG(WARNING) << "EventBus: No handler for topic "
                 << static_cast<int>(message.topic);
    return;
  }

  for (auto* handler : it->second) {
    auto response = handler->HandleMessage(message);

    // 如果是 Request 且处理器返回了响应，发送响应
    if (message.msg_type == MessageType::kRequest && response && transport_) {
      auto resp_msg = Message::Response(
          message.msg_id,
          message.topic,
          response->error_code,
          std::move(response->data));

      transport_->Send(resp_msg);
    }
  }
}

void EventBus::SetState(EventBusState state) {
  if (state_ == state) {
    return;
  }

  state_ = state;
  NotifyListeners(state);
}

void EventBus::NotifyListeners(EventBusState state) {
  for (auto* listener : listeners_) {
    listener->OnStateChanged(state);
  }
}

void EventBus::NotifyHandshakeComplete() {
  for (auto* listener : listeners_) {
    listener->OnHandshakeComplete();
  }
}

}  // namespace eventbus
}  // namespace mz
