// Copyright 2024 Simprint
// MZ NTP (New Tab Page) 组件 - 背景动画 + 2FA 验证码面板

import {MzIpHandler} from './mz_ip.mojom-webui.js';
import type {MzIpHandlerRemote, TotpCode} from './mz_ip.mojom-webui.js';

(function(): void {
  'use strict';
  const canvas = document.getElementById('mzBgCanvas') as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const ctx = canvas.getContext('2d');
  if (!ctx) {
    return;
  }

  let width: number = 0;
  let height: number = 0;
  let time: number = 0;

  function resize(): void {
    width = canvas!.width = window.innerWidth;
    height = canvas!.height = window.innerHeight;
  }

  // 绘制六边形
  function drawHexagon(x: number, y: number, size: number, alpha: number): void {
    ctx!.beginPath();
    for (let i = 0; i < 6; i++) {
      const angle = (Math.PI / 3) * i - Math.PI / 6;
      const px = x + size * Math.cos(angle);
      const py = y + size * Math.sin(angle);
      if (i === 0) {
        ctx!.moveTo(px, py);
      } else {
        ctx!.lineTo(px, py);
      }
    }
    ctx!.closePath();
    ctx!.strokeStyle = 'rgba(99, 102, 241, ' + alpha + ')';
    ctx!.lineWidth = 1;
    ctx!.stroke();
  }

  // 绘制六边形网格
  function drawHexGrid(): void {
    const size = 40;
    const h = size * Math.sqrt(3);
    const cols = Math.ceil(width / (size * 1.5)) + 2;
    const rows = Math.ceil(height / h) + 2;

    for (let row = 0; row < rows; row++) {
      for (let col = 0; col < cols; col++) {
        const x = col * size * 1.5;
        const y = row * h + (col % 2 ? h / 2 : 0);

        const cx = width / 2;
        const cy = height / 2;
        const dist = Math.sqrt((x - cx) ** 2 + (y - cy) ** 2);
        const maxDist = Math.sqrt(cx ** 2 + cy ** 2);
        const baseAlpha = 0.04 - (dist / maxDist) * 0.03;
        const breathe = Math.sin(time * 0.001 + dist * 0.005) * 0.015;
        const alpha = Math.max(0, baseAlpha + breathe);

        if (alpha > 0.01) {
          drawHexagon(x, y, size, alpha);
        }
      }
    }
  }

  // 绘制流动光带 - 从左上到右下
  function drawLightStreaks(): void {
    const streaks = [
      {offset: -0.2, width: 300, alpha: 0.025, speed: 0.0003},
      {offset: 0.3, width: 400, alpha: 0.035, speed: 0.00025},
      {offset: 0.8, width: 250, alpha: 0.02, speed: 0.00035},
    ];

    streaks.forEach((s, i) => {
      const x1 = -width * 0.3;
      const y1 = height * s.offset - height * 0.3;
      const x2 = width * 1.3;
      const y2 = height * s.offset + height * 0.7;

      const gradient = ctx!.createLinearGradient(x1, y1, x2, y2);
      const pos = (Math.sin(time * s.speed + i * 1.5) + 1) / 2;

      gradient.addColorStop(0, 'rgba(99, 102, 241, 0)');
      gradient.addColorStop(Math.max(0, pos - 0.2), 'rgba(99, 102, 241, 0)');
      gradient.addColorStop(pos, 'rgba(139, 92, 246, ' + s.alpha + ')');
      gradient.addColorStop(Math.min(1, pos + 0.2), 'rgba(99, 102, 241, 0)');
      gradient.addColorStop(1, 'rgba(99, 102, 241, 0)');

      const angle = Math.atan2(y2 - y1, x2 - x1);
      const perpX = Math.sin(angle) * s.width / 2;
      const perpY = -Math.cos(angle) * s.width / 2;

      ctx!.beginPath();
      ctx!.moveTo(x1 + perpX, y1 + perpY);
      ctx!.lineTo(x2 + perpX, y2 + perpY);
      ctx!.lineTo(x2 - perpX, y2 - perpY);
      ctx!.lineTo(x1 - perpX, y1 - perpY);
      ctx!.closePath();
      ctx!.fillStyle = gradient;
      ctx!.fill();
    });
  }

  function draw(): void {
    ctx!.clearRect(0, 0, width, height);
    drawHexGrid();
    drawLightStreaks();
    time += 16;
    requestAnimationFrame(draw);
  }

  resize();
  window.addEventListener('resize', resize);
  draw();
})();

// ===== MZ 2FA 验证码面板 =====
// 环境绑定的账号若配置了 2FA 密钥，在新标签页左下角显示当前验证码，点击复制。
// 密钥只保存在浏览器进程内存，页面只拿到计算好的验证码。
(function(): void {
  let handler: MzIpHandlerRemote|null = null;
  try {
    handler = MzIpHandler.getRemote();
  } catch (e) {
    return;
  }

  const style = document.createElement('style');
  style.textContent = `
    #mzTotpPanel {
      position: fixed; left: 16px; bottom: 16px; z-index: 1000;
      min-width: 240px; max-width: 320px; max-height: 60vh; overflow-y: auto;
      padding: 10px 12px; border-radius: 12px;
      background: rgba(20, 24, 36, 0.78); color: #fff;
      font: 13px/1.4 system-ui, -apple-system, 'Segoe UI', sans-serif;
      backdrop-filter: blur(8px); box-shadow: 0 4px 18px rgba(0,0,0,0.25);
    }
    #mzTotpPanel .mz-totp-title {
      font-size: 12px; opacity: 0.7; margin-bottom: 6px;
    }
    #mzTotpPanel .mz-totp-row {
      display: flex; align-items: center; gap: 10px; padding: 6px 4px;
      border-radius: 8px; cursor: pointer;
    }
    #mzTotpPanel .mz-totp-row:hover { background: rgba(255,255,255,0.08); }
    #mzTotpPanel .mz-totp-info { flex: 1; min-width: 0; }
    #mzTotpPanel .mz-totp-site, #mzTotpPanel .mz-totp-user {
      white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    }
    #mzTotpPanel .mz-totp-user { font-size: 11px; opacity: 0.65; }
    #mzTotpPanel .mz-totp-code {
      font: 600 18px/1 ui-monospace, 'Cascadia Mono', Consolas, monospace;
      letter-spacing: 1px;
    }
    #mzTotpPanel .mz-totp-left { font-size: 11px; opacity: 0.7; width: 26px;
      text-align: right; }
    #mzTotpPanel .mz-totp-left.mz-warn { color: #ff8a80; opacity: 1; }
    #mzTotpPanel .mz-totp-copied { color: #81c784; font-size: 11px; }
  `;
  document.head.appendChild(style);

  const panel = document.createElement('div');
  panel.id = 'mzTotpPanel';
  panel.hidden = true;
  document.body.appendChild(panel);

  let copiedKey = '';
  let copiedUntil = 0;

  function formatCode(code: string): string {
    return code.length === 6 ? code.slice(0, 3) + ' ' + code.slice(3) : code;
  }

  function render(codes: TotpCode[]): void {
    if (codes.length === 0) {
      panel.hidden = true;
      return;
    }
    panel.hidden = false;
    panel.textContent = '';
    const title = document.createElement('div');
    title.className = 'mz-totp-title';
    title.textContent = '2FA 验证码（点击复制）';
    panel.appendChild(title);
    const now = Date.now();
    for (const item of codes) {
      const key = item.site + '|' + item.username;
      const row = document.createElement('div');
      row.className = 'mz-totp-row';
      row.title = item.site + ' / ' + item.username;
      const info = document.createElement('div');
      info.className = 'mz-totp-info';
      const site = document.createElement('div');
      site.className = 'mz-totp-site';
      site.textContent = item.site;
      const user = document.createElement('div');
      user.className = 'mz-totp-user';
      if (copiedKey === key && now < copiedUntil) {
        user.className = 'mz-totp-user mz-totp-copied';
        user.textContent = '已复制';
      } else {
        user.textContent = item.username;
      }
      info.appendChild(site);
      info.appendChild(user);
      const code = document.createElement('div');
      code.className = 'mz-totp-code';
      code.textContent = formatCode(item.code);
      const left = document.createElement('div');
      left.className = item.remaining <= 5 ? 'mz-totp-left mz-warn' :
                                             'mz-totp-left';
      left.textContent = item.remaining + 's';
      row.appendChild(info);
      row.appendChild(code);
      row.appendChild(left);
      row.addEventListener('click', () => {
        navigator.clipboard.writeText(item.code).then(() => {
          copiedKey = key;
          copiedUntil = Date.now() + 1500;
          refresh();
        }, () => {});
      });
      panel.appendChild(row);
    }
  }

  let busy = false;
  function refresh(): void {
    if (busy || !handler || document.hidden) {
      return;
    }
    busy = true;
    handler.getTotpCodes().then(({codes}) => {
      render(codes);
    }, () => {
      panel.hidden = true;
    }).finally(() => {
      busy = false;
    });
  }

  refresh();
  setInterval(refresh, 1000);
  document.addEventListener('visibilitychange', refresh);
})();
