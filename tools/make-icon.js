// Renders web/icon-180.png (same motif as web/icon.svg) without external dependencies.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const S = 180, SS = 4, scale = S / 128;
function inside(x, y) {
  // x, y in 128-unit icon coordinates; returns [r,g,b,a] or null
  const rr = 28, w = 128;
  const cx = Math.min(Math.max(x, rr), w - rr), cy = Math.min(Math.max(y, rr), w - rr);
  if ((x - cx) ** 2 + (y - cy) ** 2 > rr * rr) return null;
  let col = [11, 13, 16];
  const d = Math.hypot(x - 50, y - 64);
  if (Math.abs(d - 34) <= 2) col = mix(col, [255, 255, 255], 0.35);
  if (d <= 26) col = [255, 255, 255];
  // two arcs approximated by circles through the quadratic curves
  const arc = (x0, y0, r, alpha) => { const dd = Math.hypot(x - x0, y - y0); if (Math.abs(dd - r) <= 3.5 && x > x0 + r * 0.55) col = mix(col, [77, 163, 255], alpha); };
  arc(72, 64, 24, 1.0);
  arc(78, 64, 33, 0.6);
  return col;
}
function mix(a, b, t) { return a.map((v, i) => v + (b[i] - v) * t); }

const raw = Buffer.alloc((S * 4 + 1) * S);
for (let y = 0; y < S; y++) {
  raw[y * (S * 4 + 1)] = 0;
  for (let x = 0; x < S; x++) {
    let r = 0, g = 0, b = 0, a = 0;
    for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) {
      const c = inside((x + (sx + 0.5) / SS) / scale, (y + (sy + 0.5) / SS) / scale);
      if (c) { r += c[0]; g += c[1]; b += c[2]; a += 255; }
    }
    const n = SS * SS, o = y * (S * 4 + 1) + 1 + x * 4;
    const al = a / n;
    raw[o] = al ? Math.round(r / (a / 255)) : 0; raw[o + 1] = al ? Math.round(g / (a / 255)) : 0;
    raw[o + 2] = al ? Math.round(b / (a / 255)) : 0; raw[o + 3] = Math.round(al);
  }
}
const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (buf) => { let c = 0xffffffff; for (const v of buf) c = crcTable[(c ^ v) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
  return Buffer.concat([len, td, c]);
}
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(S, 0); ihdr.writeUInt32BE(S, 4); ihdr[8] = 8; ihdr[9] = 6;
const png = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
fs.writeFileSync(path.join(__dirname, '..', 'web', 'icon-180.png'), png);
console.log('icon-180.png', png.length, 'bytes');
