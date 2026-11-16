// MZ 2FA（TOTP）验证码存储实现。

#include "mz/totp/totp_store.h"

#include <array>
#include <cctype>
#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/hash/sha1.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "url/gurl.h"

// SAFETY: 只对定长的 HMAC 块和摘要做拷贝，长度都已检查。
#pragma allow_unsafe_buffers

namespace mz::totp {

namespace {

struct ParsedSecret {
  std::vector<uint8_t> key;
  int digits = 6;
  int period = 30;
};

struct Store {
  base::Lock lock;
  std::vector<Account> accounts;
};

Store& GetStore() {
  static base::NoDestructor<Store> store;
  return *store;
}

// 简单的 URL 百分号解码（只用于 otpauth URI 的查询参数）。
std::string PercentDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int v = 0;
      if (base::HexStringToInt(s.substr(i + 1, 2), &v)) {
        out.push_back(static_cast<char>(v));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

// 解析 Base32 密钥或 otpauth://totp/...?secret=...&digits=...&period=...
bool ParseSecret(const std::string& raw, ParsedSecret* out) {
  std::string input(base::TrimWhitespaceASCII(raw, base::TRIM_ALL));
  std::string secret = input;
  if (base::StartsWith(input, "otpauth://",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    size_t q = input.find('?');
    if (q == std::string::npos) {
      return false;
    }
    secret.clear();
    std::string query = input.substr(q + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
      size_t amp = query.find('&', pos);
      if (amp == std::string::npos) {
        amp = query.size();
      }
      std::string pair = query.substr(pos, amp - pos);
      size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        std::string key = base::ToLowerASCII(pair.substr(0, eq));
        std::string value = PercentDecode(pair.substr(eq + 1));
        int n = 0;
        if (key == "secret") {
          secret = value;
        } else if (key == "digits" && base::StringToInt(value, &n) &&
                   n >= 6 && n <= 8) {
          out->digits = n;
        } else if (key == "period" && base::StringToInt(value, &n) &&
                   n > 0 && n <= 300) {
          out->period = n;
        }
      }
      pos = amp + 1;
    }
  }
  return DecodeBase32(secret, &out->key) && !out->key.empty();
}

std::array<uint8_t, base::kSHA1Length> HmacSha1(
    const std::vector<uint8_t>& key,
    const std::array<uint8_t, 8>& message) {
  constexpr size_t kBlock = 64;
  std::array<uint8_t, kBlock> k{};
  if (key.size() > kBlock) {
    base::SHA1Digest d = base::SHA1Hash(base::span<const uint8_t>(key));
    std::copy(d.begin(), d.end(), k.begin());
  } else if (!key.empty()) {
    std::copy(key.begin(), key.end(), k.begin());
  }
  std::vector<uint8_t> inner(kBlock + message.size());
  std::vector<uint8_t> outer(kBlock + base::kSHA1Length);
  for (size_t i = 0; i < kBlock; ++i) {
    inner[i] = k[i] ^ 0x36;
    outer[i] = k[i] ^ 0x5c;
  }
  std::copy(message.begin(), message.end(), inner.begin() + kBlock);
  base::SHA1Digest ih = base::SHA1Hash(base::span<const uint8_t>(inner));
  std::copy(ih.begin(), ih.end(), outer.begin() + kBlock);
  base::SHA1Digest oh = base::SHA1Hash(base::span<const uint8_t>(outer));
  std::array<uint8_t, base::kSHA1Length> result;
  std::copy(oh.begin(), oh.end(), result.begin());
  return result;
}

std::string SiteLabel(const std::string& url) {
  GURL gurl(url);
  if (gurl.is_valid() && !gurl.host().empty()) {
    std::string host(gurl.host());
    if (base::StartsWith(host, "www.", base::CompareCase::SENSITIVE)) {
      host = host.substr(4);
    }
    return host;
  }
  return url;
}

}  // namespace

bool DecodeBase32(const std::string& input, std::vector<uint8_t>* out) {
  out->clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : input) {
    if (c == ' ' || c == '-' || c == '=') {
      continue;
    }
    int value;
    char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u >= 'A' && u <= 'Z') {
      value = u - 'A';
    } else if (u >= '2' && u <= '7') {
      value = u - '2' + 26;
    } else {
      return false;
    }
    buffer = (buffer << 5) | static_cast<uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
    }
  }
  return true;
}

std::string ComputeCode(const std::vector<uint8_t>& key,
                        uint64_t counter,
                        int digits) {
  std::array<uint8_t, 8> msg;
  for (int i = 7; i >= 0; --i) {
    msg[i] = static_cast<uint8_t>(counter & 0xff);
    counter >>= 8;
  }
  std::array<uint8_t, base::kSHA1Length> h = HmacSha1(key, msg);
  int offset = h[h.size() - 1] & 0x0f;
  uint32_t bin = (static_cast<uint32_t>(h[offset] & 0x7f) << 24) |
                 (static_cast<uint32_t>(h[offset + 1]) << 16) |
                 (static_cast<uint32_t>(h[offset + 2]) << 8) |
                 static_cast<uint32_t>(h[offset + 3]);
  uint32_t mod = 1;
  for (int i = 0; i < digits; ++i) {
    mod *= 10;
  }
  std::string code = base::NumberToString(bin % mod);
  while (static_cast<int>(code.size()) < digits) {
    code.insert(code.begin(), '0');
  }
  return code;
}

void SetAccounts(std::vector<Account> accounts) {
  Store& store = GetStore();
  base::AutoLock lock(store.lock);
  store.accounts = std::move(accounts);
}

std::vector<Code> GetCodes(base::Time now) {
  std::vector<Account> accounts;
  {
    Store& store = GetStore();
    base::AutoLock lock(store.lock);
    accounts = store.accounts;
  }
  const int64_t unix_seconds =
      (now - base::Time::UnixEpoch()).InSeconds();
  std::vector<Code> codes;
  for (const Account& account : accounts) {
    ParsedSecret parsed;
    if (!ParseSecret(account.secret, &parsed)) {
      continue;
    }
    Code code;
    code.site = SiteLabel(account.url);
    code.username = account.username;
    code.period = parsed.period;
    code.code = ComputeCode(parsed.key,
                            static_cast<uint64_t>(unix_seconds / parsed.period),
                            parsed.digits);
    code.remaining =
        parsed.period - static_cast<int32_t>(unix_seconds % parsed.period);
    codes.push_back(std::move(code));
  }
  return codes;
}

}  // namespace mz::totp
