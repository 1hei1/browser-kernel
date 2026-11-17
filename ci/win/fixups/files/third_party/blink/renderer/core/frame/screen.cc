/*
 * Copyright (C) 2007 Apple Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/frame/screen.h"

#include "base/numerics/safe_conversions.h"
#include "services/network/public/mojom/permissions_policy/permissions_policy_feature.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/event_target_names.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "ui/display/screen_info.h"
#include "ui/display/screen_infos.h"
#include "mz/fingerprint/screen_hardware_fingerprint.h"

namespace blink {

Screen::Screen(LocalDOMWindow* window, int64_t display_id)
    : ExecutionContextClient(window), display_id_(display_id) {}

// static
bool Screen::AreWebExposedScreenPropertiesEqual(
    const display::ScreenInfo& prev,
    const display::ScreenInfo& current) {
  // height() and width() use rect.size()
  if (prev.rect.size() != current.rect.size()) {
    return false;
  }

  // height() and width() use device_scale_factor
  // Note: comparing device_scale_factor is a bit of a lie as Screen only uses
  // this with the PhysicalPixelsQuirk (see width() / height() below).  However,
  // this value likely changes rarely and should not throw many false positives.
  if (prev.device_scale_factor != current.device_scale_factor) {
    return false;
  }

  // avail[Left|Top|Width|Height](), height() and width() use
  // text_scale_multiplier Note: similar to device_scale_factor,
  // text_scale_multiplier is only used in select scenarios, but is unlikely to
  // change and should not result in many false positives.
  if (prev.text_scale_multiplier != current.text_scale_multiplier) {
    return false;
  }

  // avail[Left|Top|Width|Height]() use available_rect
  if (prev.available_rect != current.available_rect) {
    return false;
  }

  // colorDepth() and pixelDepth() use depth
  if (prev.depth != current.depth) {
    return false;
  }

  // isExtended()
  if (prev.is_extended != current.is_extended) {
    return false;
  }

  return true;
}

int Screen::height() const {
  // MZ: 应用屏幕高度指纹
  int configured_height = mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenHeight();
  if (configured_height > 0) {
    return configured_height;
  }

  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/false).height();
}

int Screen::width() const {
  // MZ: 应用屏幕宽度指纹
  int configured_width = mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenWidth();
  if (configured_width > 0) {
    return configured_width;
  }

  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/false).width();
}

unsigned Screen::colorDepth() const {
  // MZ: 应用色深指纹
  int configured_color_depth = mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredColorDepth();
  if (configured_color_depth > 0) {
    return static_cast<unsigned>(configured_color_depth);
  }

  // "If the user agent does not know the color depth or does not want to
  // return it for privacy considerations, it should return 24."
  //
  // https://drafts.csswg.org/cssom-view/#dom-screen-colordepth
  unsigned unknown_color_depth = 24u;

  if (!DomWindow()) {
    return unknown_color_depth;
  }
  return GetScreenInfo().depth == 0
             ? unknown_color_depth
             : base::saturated_cast<unsigned>(GetScreenInfo().depth);
}

unsigned Screen::pixelDepth() const {
  return colorDepth();
}

int Screen::availLeft() const {
  // MZ: 伪装成单屏、任务栏在底部：可用区域从屏幕左上角开始
  if (mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenWidth() >
      0) {
    return 0;
  }
  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/true).x();
}

int Screen::availTop() const {
  // MZ: 伪装成单屏、任务栏在底部：可用区域从屏幕左上角开始
  if (mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenWidth() >
      0) {
    return 0;
  }
  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/true).y();
}

int Screen::availHeight() const {
  // MZ: 可用高度 = 屏幕高度 - 任务栏。真实 Windows 的 availHeight 小于 height；
  // 两者相等意味着没有任务栏，是明显的异常特征。
  int configured_height = mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenHeight();
  if (configured_height > 0) {
    constexpr int kWindowsTaskbarHeight = 40;
    return configured_height > 2 * kWindowsTaskbarHeight
               ? configured_height - kWindowsTaskbarHeight
               : configured_height;
  }

  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/true).height();
}

int Screen::availWidth() const {
  // MZ: 应用屏幕宽度指纹（availWidth 应该与 width 一致）
  int configured_width = mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenWidth();
  if (configured_width > 0) {
    return configured_width;
  }

  if (!DomWindow())
    return 0;
  return GetRect(/*available=*/true).width();
}

void Screen::Trace(Visitor* visitor) const {
  EventTarget::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
  Supplementable<Screen>::Trace(visitor);
}

const AtomicString& Screen::InterfaceName() const {
  return event_target_names::kScreen;
}

ExecutionContext* Screen::GetExecutionContext() const {
  return ExecutionContextClient::GetExecutionContext();
}

bool Screen::isExtended() const {
  if (!DomWindow()) {
    return false;
  }
  // MZ: 伪装的是单块屏幕，多显示器标记不能露出来
  if (mz::fingerprint::ScreenHardwareFingerprint::GetConfiguredScreenWidth() >
      0) {
    return false;
  }
  auto* context = GetExecutionContext();
  if (!context->IsFeatureEnabled(
          network::mojom::PermissionsPolicyFeature::kWindowManagement)) {
    return false;
  }

  return GetScreenInfo().is_extended;
}

gfx::Rect Screen::GetRect(bool available) const {
  if (!DomWindow())
    return gfx::Rect();
  LocalFrame* frame = DomWindow()->GetFrame();
  const display::ScreenInfo& screen_info = GetScreenInfo();
  gfx::Rect rect = available ? screen_info.available_rect : screen_info.rect;
  if (frame->GetSettings()->GetReportScreenSizeInPhysicalPixelsQuirk())
    return gfx::ScaleToRoundedRect(rect, screen_info.device_scale_factor);
  if (frame->GetDocument() && frame->GetDocument()->TextScaleMetaTagPresent()) {
    return gfx::ScaleToRoundedRect(rect, screen_info.text_scale_multiplier);
  }
  return rect;
}

const display::ScreenInfo& Screen::GetScreenInfo() const {
  DCHECK(DomWindow());
  LocalFrame* frame = DomWindow()->GetFrame();

  const auto& screen_infos = frame->GetChromeClient().GetScreenInfos(*frame);
  for (const auto& screen : screen_infos.screen_infos) {
    if (screen.display_id == display_id_)
      return screen;
  }
  DEFINE_STATIC_LOCAL(display::ScreenInfo, kEmptyScreenInfo, ());
  return kEmptyScreenInfo;
}

}  // namespace blink
