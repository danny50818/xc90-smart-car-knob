#include "SmartCarKnobBoard.h"
#include "UiDesignV12.h"

#include <Wire.h>
#include <cmath>
#include <cstring>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>

namespace {
constexpr int LCD_HOST = SPI2_HOST;
constexpr gpio_num_t LCD_CS = GPIO_NUM_12;
constexpr gpio_num_t LCD_CLK = GPIO_NUM_10;
// spi_bus_config_t data0..data3 map to ESP32-S3 D/Q/WP/HD respectively.
constexpr gpio_num_t LCD_D0 = GPIO_NUM_13;
constexpr gpio_num_t LCD_D1 = GPIO_NUM_11;
constexpr gpio_num_t LCD_D2 = GPIO_NUM_14;
constexpr gpio_num_t LCD_D3 = GPIO_NUM_9;
constexpr gpio_num_t LCD_RST = GPIO_NUM_8;
constexpr gpio_num_t LCD_BL = GPIO_NUM_17;

constexpr uint8_t TOUCH_ADDR = 0x15;
constexpr gpio_num_t TOUCH_SDA = GPIO_NUM_1;
constexpr gpio_num_t TOUCH_SCL = GPIO_NUM_3;
constexpr gpio_num_t TOUCH_RST = GPIO_NUM_2;
constexpr gpio_num_t TOUCH_INT = GPIO_NUM_4;

constexpr int W = UiV12::WIDTH;
constexpr int H = UiV12::HEIGHT;
constexpr int LCD_X_OFFSET = 6;
constexpr int LCD_TRANSFER_W = W;
constexpr int STRIP_ROWS = 20;
constexpr size_t PIXELS = static_cast<size_t>(W) * H;
constexpr size_t FRAME_BYTES = PIXELS * sizeof(uint16_t);
constexpr size_t STRIP_BYTES = static_cast<size_t>(LCD_TRANSFER_W) * STRIP_ROWS * sizeof(uint16_t);
constexpr float PI_F = 3.14159265358979323846f;

esp_lcd_panel_io_handle_t lcdIo = nullptr;
SemaphoreHandle_t frameFree = nullptr;
uint16_t* frame = nullptr;
uint16_t* dmaStrip = nullptr;
uint16_t* transitionOld = nullptr;
uint16_t* transitionNew = nullptr;
bool displayReady = false;
volatile bool displayAwakeRequested = true;
bool displayAwake = true;
bool touchReady = false;
uint32_t boardStartedAt = 0;
uint32_t lastFrameAt = 0;
uint32_t transitionStartedAt = 0;
int8_t transitionDirection = 1;
bool transitionFromBoot = false;

uint16_t color565(uint32_t rgb) {
  const uint16_t value = ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) |
                         ((rgb >> 3) & 0x001F);
  return __builtin_bswap16(value);
}

uint32_t mix(uint32_t a, uint32_t b, uint8_t amount) {
  const uint8_t inv = 255 - amount;
  const uint8_t r = ((((a >> 16) & 0xff) * inv) + (((b >> 16) & 0xff) * amount)) / 255;
  const uint8_t g = ((((a >> 8) & 0xff) * inv) + (((b >> 8) & 0xff) * amount)) / 255;
  const uint8_t bl = (((a & 0xff) * inv) + ((b & 0xff) * amount)) / 255;
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | bl;
}

void clear(uint32_t rgb) {
  const uint16_t c = color565(rgb);
  for (size_t i = 0; i < PIXELS; ++i) frame[i] = c;
}

void pixel(int x, int y, uint32_t rgb) {
  if (static_cast<unsigned>(x) >= W || static_cast<unsigned>(y) >= H) return;
  frame[static_cast<size_t>(y) * W + x] = color565(rgb);
}

void fillCircle(int cx, int cy, int radius, uint32_t rgb) {
  const int rr = radius * radius;
  const uint16_t c = color565(rgb);
  for (int y = -radius; y <= radius; ++y) {
    const int span = static_cast<int>(sqrtf(static_cast<float>(rr - y * y)));
    const int yy = cy + y;
    if (yy < 0 || yy >= H) continue;
    const int from = max(0, cx - span);
    const int to = min(W - 1, cx + span);
    for (int x = from; x <= to; ++x) frame[static_cast<size_t>(yy) * W + x] = c;
  }
}

void fillRect(int x0, int y0, int x1, int y1, uint32_t rgb) {
  x0 = max(0, x0); y0 = max(0, y0); x1 = min(W - 1, x1); y1 = min(H - 1, y1);
  const uint16_t c = color565(rgb);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) frame[static_cast<size_t>(y) * W + x] = c;
}

void line(int x0, int y0, int x1, int y1, uint32_t rgb, int width = 1) {
  const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (width <= 1) pixel(x0, y0, rgb);
    else fillCircle(x0, y0, width / 2, rgb);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void ring(int cx, int cy, int radius, uint32_t rgb, int width = 2) {
  const int steps = max(48, radius * 4);
  for (int i = 0; i <= steps; ++i) {
    const float a = (2.0f * PI_F * i) / steps;
    fillCircle(cx + lroundf(cosf(a) * radius), cy + lroundf(sinf(a) * radius),
               max(1, width / 2), rgb);
  }
}

void arc(int cx, int cy, int radius, int fromDeg, int toDeg, uint32_t rgb, int width) {
  for (int deg = fromDeg; deg <= toDeg; ++deg) {
    const float a = deg * PI_F / 180.0f;
    fillCircle(cx + lroundf(cosf(a) * radius), cy + lroundf(sinf(a) * radius),
               max(1, width / 2), rgb);
  }
}

void glowRing(int cx, int cy, int radius, uint32_t rgb, int width) {
  ring(cx, cy, radius - width, mix(rgb, 0, 205), 3);
  ring(cx, cy, radius + width, mix(rgb, 0, 215), 4);
  ring(cx, cy, radius, rgb, width);
}

void glowLine(int x0, int y0, int x1, int y1, uint32_t rgb, int width) {
  line(x0, y0, x1, y1, mix(rgb, 0, 210), width + 8);
  line(x0, y0, x1, y1, mix(rgb, 0, 150), width + 3);
  line(x0, y0, x1, y1, rgb, width);
}

void transformedBlit(const uint16_t* source, float scale, float shiftX, float opacity) {
  static constexpr uint8_t bayer[4][4] = {
    {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}
  };
  const int opacityLevel = constrain(lroundf(opacity * 16.0f), 0L, 16L);
  if (!opacityLevel) return;
  // 16.16 fixed-point inverse transform avoids two floating-point rounds for
  // every one of the 217k pixels during a page transition.
  const int32_t inverse = lroundf(65536.0f / scale);
  const int32_t shift = lroundf(shiftX * 65536.0f);
  for (int y = 0; y < H; ++y) {
    const int sy = 233 + static_cast<int>((static_cast<int64_t>(y - 233) * inverse) >> 16);
    if (sy < 0 || sy >= H) continue;
    int32_t sourceX = static_cast<int32_t>(((-233LL * 65536 - shift) * inverse) >> 16) + 233 * 65536;
    for (int x = 0; x < W; ++x) {
      const int sx = sourceX >> 16;
      if ((opacityLevel >= 16 || bayer[y & 3][x & 3] < opacityLevel) && sx >= 0 && sx < W)
        frame[static_cast<size_t>(y) * W + x] = source[static_cast<size_t>(sy) * W + sx];
      sourceX += inverse;
    }
  }
}

void composeTransition(float progress) {
  progress = constrain(progress, 0.0f, 1.0f);
  if (transitionFromBoot) {
    memcpy(frame, transitionNew, FRAME_BYTES);
    transformedBlit(transitionOld, 1.0f + 0.1f * progress, 0, 1.0f - progress);
    return;
  }
  clear(UiV12::BLACK);
  const float direction = transitionDirection >= 0 ? 1.0f : -1.0f;
  transformedBlit(transitionOld, 1.0f - 0.15f * progress,
                  -direction * W * 0.5f * progress, 1.0f - progress);
  transformedBlit(transitionNew, 0.85f + 0.15f * progress,
                  direction * W * 0.5f * (1.0f - progress), progress);
}

const uint8_t* glyph(char c) {
  static const uint8_t blank[5] = {0,0,0,0,0};
  static const uint8_t digits[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e}
  };
  if (c >= '0' && c <= '9') return digits[c - '0'];
  static const uint8_t A[5]={0x7e,0x11,0x11,0x11,0x7e}, B[5]={0x7f,0x49,0x49,0x49,0x36};
  static const uint8_t C[5]={0x3e,0x41,0x41,0x41,0x22}, D[5]={0x7f,0x41,0x41,0x22,0x1c};
  static const uint8_t E[5]={0x7f,0x49,0x49,0x49,0x41}, Hh[5]={0x7f,0x08,0x08,0x08,0x7f};
  static const uint8_t I[5]={0x00,0x41,0x7f,0x41,0x00}, M[5]={0x7f,0x02,0x0c,0x02,0x7f};
  static const uint8_t N[5]={0x7f,0x04,0x08,0x10,0x7f}, O[5]={0x3e,0x41,0x41,0x41,0x3e};
  static const uint8_t S[5]={0x46,0x49,0x49,0x49,0x31}, T[5]={0x01,0x01,0x7f,0x01,0x01};
  static const uint8_t U[5]={0x3f,0x40,0x40,0x40,0x3f}, V[5]={0x1f,0x20,0x40,0x20,0x1f};
  static const uint8_t Y[5]={0x07,0x08,0x70,0x08,0x07};
  static const uint8_t PERCENT[5]={0x63,0x13,0x08,0x64,0x63};
  switch (c) {
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
    case 'E': return E; case 'H': return Hh; case 'I': return I; case 'M': return M;
    case 'N': return N; case 'O': return O; case 'S': return S; case 'T': return T;
    case 'U': return U; case 'V': return V; case 'Y': return Y; case '%': return PERCENT;
    default: return blank;
  }
}

void text(const char* value, int x, int y, int scale, uint32_t rgb) {
  for (; *value; ++value, x += 6 * scale) {
    const uint8_t* g = glyph(*value);
    for (int col = 0; col < 5; ++col)
      for (int row = 0; row < 7; ++row)
        if (g[col] & (1 << row))
          for (int dx = 0; dx < scale; ++dx)
            for (int dy = 0; dy < scale; ++dy) pixel(x + col * scale + dx, y + row * scale + dy, rgb);
  }
}

void centeredText(const char* value, int y, int scale, uint32_t rgb) {
  const int width = strlen(value) * 6 * scale - scale;
  text(value, (W - width) / 2, y, scale, rgb);
}

void ghostFibers(uint32_t now) {
  // Literal 320px Canvas math from the HTML, scaled by 466/320.
  const float t = now * 0.00090f;
  const float angle = (-58.0f * PI_F / 180.0f) + t * 0.03f;
  const float ca = cosf(angle), sa = sinf(angle);
  constexpr float canvasScale = UiV12::MOCKUP_SCALE * 2.4f;
  for (int layer = 0; layer < 2; ++layer) {
    // HTML effective daytime alpha: gray .3*.6, blue .5*.6.
    const uint32_t color = layer == 0 ? 0x121212 : 0x0e1b4c;
    const int width = layer == 0 ? 3 : 5;
    for (int l = 0; l < 2; ++l) {
      for (int track = 0; track < 2; ++track) {
        int px = 0, py = 0; bool first = true;
        for (int x = -100; x <= 100; x += 10) {
          const float offset = (track - 1) * (layer == 0 ? 15.0f : 8.0f);
          const float y = layer == 0
            ? offset + sinf(x * 0.03f + t * 0.7f + l) * 25.0f + cosf(x * 0.05f - t * 0.6f + track) * 12.0f
            : offset + sinf(x * 0.04f + t + l * 2.0f) * 20.0f + cosf(x * 0.03f - t * 0.8f + track * 0.5f) * 10.0f;
          const int xx = 233 + lroundf((x * ca - y * sa) * canvasScale);
          const int yy = 233 + lroundf((x * sa + y * ca) * canvasScale);
          if (!first) line(px, py, xx, yy, color, width);
          px = xx; py = yy; first = false;
        }
      }
    }
  }
}

void shadeNavigationSector(uint8_t focus) {
  if (focus > 3) return;
  const uint32_t accent = (focus == 0 || focus == 2) ? UiV12::CYAN : UiV12::PINK;
  for (int y = 0; y < H; ++y) {
    const int dy = y - 233;
    for (int x = 0; x < W; ++x) {
      const int dx = x - 233;
      bool inside = false;
      if (focus == 0) inside = dy <= 0 && abs(dx) <= -dy;
      if (focus == 1) inside = dx >= 0 && abs(dy) <= dx;
      if (focus == 2) inside = dy >= 0 && abs(dx) <= dy;
      if (focus == 3) inside = dx <= 0 && abs(dy) <= -dx;
      if (!inside) continue;
      const int radial = min(233, static_cast<int>(sqrtf(static_cast<float>(dx * dx + dy * dy))));
      const uint8_t amount = radial < 47 ? 0 : static_cast<uint8_t>((radial - 47) * 48 / 186);
      if (amount) frame[static_cast<size_t>(y) * W + x] = color565(mix(0x000000, accent, amount));
    }
  }
}

void drawBoot(uint32_t now) {
  clear(UiV12::BLACK);
  const uint32_t elapsed = now - boardStartedAt;
  const float outer = min(1.0f, elapsed / 1000.0f);
  const float inner = elapsed < 100 ? 0.0f : min(1.0f, (elapsed - 100) / 800.0f);
  if (outer > 0) arc(233,233,157,-90,-90 + lroundf(360 * outer),mix(UiV12::CYAN,0,180),17);
  if (outer > 0) arc(233,233,157,-90,-90 + lroundf(360 * outer),UiV12::CYAN,9);
  if (inner > 0) arc(233,233,105,90,90 + lroundf(360 * inner),mix(UiV12::PINK,0,175),22);
  if (inner > 0) arc(233,233,105,90,90 + lroundf(360 * inner),UiV12::PINK,12);
  if (elapsed >= 500) {
    float p = min(1.0f, (elapsed - 500) / 400.0f);
    float scale = p < 0.7f ? (p / 0.7f) * 1.1f : 1.1f - ((p - 0.7f) / 0.3f) * 0.1f;
    fillCircle(233,233,lroundf(28 * scale),0x555555);
    fillCircle(233,233,lroundf(24 * scale),UiV12::WHITE);
  }
}

void drawNavigation(const KnobUiState& state) {
  shadeNavigationSector(state.focus);
  ghostFibers(millis());
  line(0,233,465,233,0x151515,3); line(233,0,233,465,0x151515,3);
  const int centers[4][2]={{233,76},{390,233},{233,390},{76,233}};
  for (int i = 0; i < 4; ++i) {
    const bool focused = state.focus == i;
    const uint32_t accent = (i == 0 || i == 2) ? UiV12::CYAN : UiV12::PINK;
    const uint32_t c = focused ? accent : 0x999999;
    const int d = focused ? 21 : 18, q = focused ? 14 : 12, cx=centers[i][0], cy=centers[i][1];
    if (i==0) { glowLine(cx-d,cy+q,cx,cy-q,c,focused?5:4); glowLine(cx,cy-q,cx+d,cy+q,c,focused?5:4); }
    if (i==1) { glowLine(cx-q,cy-d,cx+q,cy,c,focused?5:4); glowLine(cx+q,cy,cx-q,cy+d,c,focused?5:4); }
    if (i==2) { glowLine(cx-d,cy-q,cx,cy+q,c,focused?5:4); glowLine(cx,cy+q,cx+d,cy-q,c,focused?5:4); }
    if (i==3) { glowLine(cx+q,cy-d,cx-q,cy,c,focused?5:4); glowLine(cx-q,cy,cx+q,cy+d,c,focused?5:4); }
  }
  if (state.focus == 4) glowRing(233,233,61,UiV12::GREEN,4);
  fillCircle(233,233,61,state.focus==4?0x0b2515:0x080808);
  ring(233,233,61,state.focus==4?UiV12::GREEN:0x222222,3);
  const uint32_t mic = state.focus==4?UiV12::GREEN:0xb3b3b3;
  // Feather-style microphone icon from the source SVG.
  line(224,211,224,235,mic,4); line(242,211,242,235,mic,4);
  arc(233,211,9,180,360,mic,4); arc(233,235,9,0,180,mic,4);
  arc(233,233,23,0,180,mic,4); line(210,233,210,225,mic,4); line(256,233,256,225,mic,4);
  line(233,256,233,268,mic,4); line(221,268,245,268,mic,4);
}

void drawTouchpad(const KnobUiState& state) {
  clear(0x030303);
  const float wave = millis() * 0.0024f;
  for (int y = 23; y <= 443; y += UiV12::DOT_SPACING) {
    for (int x = 23; x <= 443; x += UiV12::DOT_SPACING) {
      int xx = x, yy = y;
      uint32_t c = 0x008999;
      if (state.touchPressed) {
        const int dx = state.touchX - x, dy = state.touchY - y;
        const float d = sqrtf(static_cast<float>(dx*dx + dy*dy));
        if (d < UiV12::DOT_BULGE_RADIUS && d > 1) {
          const float falloff = 1.0f - d / UiV12::DOT_BULGE_RADIUS;
          const float push = falloff * falloff * UiV12::DOT_BULGE_STRENGTH;
          xx -= lroundf(dx / d * push); yy -= lroundf(dy / d * push);
        }
      }
      yy += lroundf(sinf(x * 0.0206f + wave) * 0.7f);
      fillCircle(xx, yy, UiV12::DOT_RADIUS, c);
    }
  }
  const int barY = 116 + state.scrollPosition * 167 / 100;
  line(444,116,444,349,0x282828,6);
  glowLine(444,barY,444,min(349,barY+66),UiV12::CYAN,6);
}

void drawMedia(const KnobUiState& state) {
  static float displayedVolume = 45.0f;
  static float velocity = 0.0f;
  static uint32_t previousAt = 0;
  const uint32_t now = millis();
  const float dt = previousAt ? constrain((now - previousAt) / 1000.0f, 0.001f, 0.05f) : 0.04f;
  previousAt = now;
  const float previous = displayedVolume;
  displayedVolume += (state.volume - displayedVolume) * (1.0f - expf(-dt / UiV12::ANIMATION.volumeFollowTau));
  const float rawVelocity = ((displayedVolume - previous) / dt) / 100.0f;
  velocity += (rawVelocity - velocity) * (1.0f - expf(-dt / UiV12::ANIMATION.volumeVelocityTau));
  const float speed = min(fabsf(velocity) / 4.0f, 1.0f);
  const int direction = velocity < 0 ? -1 : 1;
  ghostFibers(millis());
  arc(233,233,UiV12::MEDIA_DIAL_RADIUS,140,400,0x171717,UiV12::MEDIA_DIAL_WIDTH);
  const int end = 140 + lroundf(260.0f * displayedVolume / 100.0f);
  arc(233,233,UiV12::MEDIA_DIAL_RADIUS,140,end,mix(UiV12::CYAN,0,170),19);
  arc(233,233,UiV12::MEDIA_DIAL_RADIUS,140,end,UiV12::CYAN,UiV12::MEDIA_DIAL_WIDTH);
  const float segment = UiV12::ANIMATION.cometReachDeg * speed / UiV12::ANIMATION.cometTailSegments;
  for (int j = UiV12::ANIMATION.cometTailSegments - 1; j >= 0; --j) {
    if (segment < 0.01f) continue;
    const float weight = 1.0f - static_cast<float>(j) / UiV12::ANIMATION.cometTailSegments;
    int a0 = direction > 0 ? lroundf(end - (j + 1) * segment) : lroundf(end + j * segment);
    int a1 = direction > 0 ? lroundf(end - j * segment) : lroundf(end + (j + 1) * segment);
    a0 = constrain(a0, 140, 400); a1 = constrain(a1, 140, 400);
    if (a0 > a1) { const int swap = a0; a0 = a1; a1 = swap; }
    if (a1 > a0) arc(233,233,UiV12::MEDIA_DIAL_RADIUS,a0,a1,
                     mix(UiV12::BLACK,UiV12::CYAN,lroundf(255 * weight * speed)),
                     lroundf(UiV12::MEDIA_DIAL_WIDTH + 15 * weight * speed));
  }
  const float ha=end*PI_F/180.0f;
  fillCircle(233+lroundf(cosf(ha)*198),233+lroundf(sinf(ha)*198),14,0x555555);
  fillCircle(233+lroundf(cosf(ha)*198),233+lroundf(sinf(ha)*198),10,UiV12::WHITE);
  char volume[5]; snprintf(volume,sizeof(volume),"%u%%",static_cast<unsigned>(lroundf(displayedVolume)));
  centeredText(volume,66,5,UiV12::WHITE);
  const int cx[3]={105,233,361}, radius[3]={44,62,44};
  for(int i=0;i<3;++i) {
    const bool f=state.focus==i;
    const uint32_t a=i==1?UiV12::RED:UiV12::CYAN;
    if(f) glowRing(cx[i],244,radius[i],a,4);
    fillCircle(cx[i],244,radius[i],f?(i==1?0x251015:0x071d21):0x080808);
    ring(cx[i],244,radius[i],f?a:0x292929,3);
  }
  const uint32_t side=state.focus==0?UiV12::CYAN:0x999999;
  line(119,220,88,244,side,5); line(88,244,119,268,side,5); line(80,222,80,266,side,5);
  const uint32_t pc=state.focus==1?UiV12::RED:0x999999;
  if(state.playing){ fillRect(216,219,225,269,pc); fillRect(241,219,250,269,pc); }
  else { line(218,216,218,272,pc,6); line(218,216,259,244,pc,6); line(259,244,218,272,pc,6); }
  const uint32_t next=state.focus==2?UiV12::CYAN:0x999999;
  line(347,220,378,244,next,5); line(378,244,347,268,next,5); line(386,222,386,266,next,5);
  const bool muteFocus=state.focus==3;
  if(muteFocus) glowRing(233,397,35,UiV12::GOLD,4);
  fillCircle(233,397,35,muteFocus?0x28220a:0x080808); ring(233,397,35,muteFocus?UiV12::GOLD:0x292929,3);
  const uint32_t mc=muteFocus?UiV12::GOLD:0x999999;
  line(215,390,224,390,mc,4); line(224,390,237,379,mc,4); line(237,379,237,415,mc,4);
  line(237,415,224,404,mc,4); line(224,404,215,404,mc,4); line(245,387,260,407,mc,4); line(260,387,245,407,mc,4);
}

void drawSystem(const KnobUiState& state) {
  ghostFibers(millis());
  line(233,0,233,465,0x262626,3); line(0,233,465,233,0x262626,3);
  const int cx[4]={138,328,138,328}, cy[4]={138,138,328,328};
  for(int i=0;i<4;++i) if(state.focus==i) {
    for(int r=68;r>4;r-=4) fillCircle(cx[i],cy[i],r,mix(0x000000,UiV12::CYAN,static_cast<uint8_t>((68-r)*45/64)));
  }
  for(int i=0;i<4;++i) {
    const uint32_t c=state.focus==i?UiV12::CYAN:0x999999; const int x=cx[i],y=cy[i];
    if(i==0){ // home
      line(x-25,y-5,x,y-27,c,5); line(x,y-27,x+25,y-5,c,5); line(x-21,y-7,x-21,y+24,c,5);
      line(x+21,y-7,x+21,y+24,c,5); line(x-21,y+24,x+21,y+24,c,5); fillRect(x-6,y+5,x+6,y+24,c);
    } else if(i==1){ // back
      line(x+26,y,x-24,y,c,5); line(x-24,y,x-4,y-21,c,5); line(x-24,y,x-4,y+21,c,5);
    } else if(i==2){ // app switch: four rounded-square approximation
      for(int oy=-18;oy<=18;oy+=36) for(int ox=-18;ox<=18;ox+=36){
        line(x+ox-10,y+oy-10,x+ox+10,y+oy-10,c,4); line(x+ox+10,y+oy-10,x+ox+10,y+oy+10,c,4);
        line(x+ox+10,y+oy+10,x+ox-10,y+oy+10,c,4); line(x+ox-10,y+oy+10,x+ox-10,y+oy-10,c,4);
      }
    } else { // power
      arc(x,y,29,-45,225,c,5); line(x,y-34,x,y+2,c,5);
    }
  }
  const uint32_t dc=state.focus==4?UiV12::GOLD:0x999999;
  if(state.focus==4){
    line(233,163,303,233,mix(UiV12::GOLD,0,190),12); line(303,233,233,303,mix(UiV12::GOLD,0,190),12);
    line(233,303,163,233,mix(UiV12::GOLD,0,190),12); line(163,233,233,163,mix(UiV12::GOLD,0,190),12);
  }
  fillCircle(233,233,48,state.focus==4?0x28220a:0x080808);
  line(233,163,303,233,dc,4); line(303,233,233,303,dc,4); line(233,303,163,233,dc,4); line(163,233,233,163,dc,4);
  // Gear icon: central circle plus eight teeth.
  ring(233,233,13,dc,5); ring(233,233,31,dc,4);
  for(int a=0;a<360;a+=45){float q=a*PI_F/180.0f; line(233+lroundf(cosf(q)*31),233+lroundf(sinf(q)*31),233+lroundf(cosf(q)*40),233+lroundf(sinf(q)*40),dc,6);}
}

bool onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(frameFree, &woken);
  return woken == pdTRUE;
}

int qspiCommand(uint8_t opcode, uint8_t command) {
  return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(command) << 8);
}

esp_err_t sendCommand(uint8_t command, const void* data = nullptr, size_t size = 0) {
  return esp_lcd_panel_io_tx_param(lcdIo, qspiCommand(0x02, command), data, size);
}

bool initDisplay() {
  // GPIO17 is the panel enable rail on this module, not merely a cosmetic
  // backlight.  The factory firmware raises it before creating the SPI bus;
  // commands sent while it is low are acknowledged by ESP-IDF but never reach
  // the unpowered SH8601.
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);
  delay(10);
  digitalWrite(LCD_BL, HIGH);
  delay(10);
  pinMode(LCD_RST, OUTPUT); digitalWrite(LCD_RST, LOW); delay(12); digitalWrite(LCD_RST, HIGH); delay(150);

  spi_bus_config_t bus{};
  bus.sclk_io_num = LCD_CLK; bus.data0_io_num = LCD_D0; bus.data1_io_num = LCD_D1;
  bus.data2_io_num = LCD_D2; bus.data3_io_num = LCD_D3;
  bus.data4_io_num = -1; bus.data5_io_num = -1; bus.data6_io_num = -1; bus.data7_io_num = -1;
  bus.max_transfer_sz = STRIP_BYTES;
  if (spi_bus_initialize(static_cast<spi_host_device_t>(LCD_HOST), &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  esp_lcd_panel_io_spi_config_t io{};
  io.cs_gpio_num = LCD_CS; io.dc_gpio_num = -1; io.spi_mode = 0; io.pclk_hz = 40 * 1000 * 1000;
  io.trans_queue_depth = 10; io.on_color_trans_done = onColorDone; io.user_ctx = nullptr;
  io.lcd_cmd_bits = 32; io.lcd_param_bits = 8; io.flags.quad_mode = true;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &lcdIo) != ESP_OK) return false;

  // Exact command table recovered from this unit's factory firmware.
  // FE and 35 intentionally carry zero parameter bytes.
  const uint8_t c4 = 0x80, colmod = 0x55, madctl = 0x00;
  const uint8_t control = 0x20, brightness = 0xff, gate = 0xff;
  const uint8_t columns[] = {0x00,0x06,0x01,0xd7};
  const uint8_t rows[] = {0x00,0x00,0x01,0xd1};
  if (sendCommand(0x36,&madctl,1) != ESP_OK || sendCommand(0x3a,&colmod,1) != ESP_OK ||
      sendCommand(0xfe) != ESP_OK || sendCommand(0xc4,&c4,1) != ESP_OK ||
      sendCommand(0x3a,&colmod,1) != ESP_OK || sendCommand(0x35) != ESP_OK) return false;
  delay(10);
  if (sendCommand(0x53,&control,1) != ESP_OK) return false;
  delay(10);
  if (sendCommand(0x51,&brightness,1) != ESP_OK) return false;
  delay(10);
  if (sendCommand(0x63,&gate,1) != ESP_OK) return false;
  delay(10);
  if (sendCommand(0x2a,columns,sizeof(columns)) != ESP_OK ||
      sendCommand(0x2b,rows,sizeof(rows)) != ESP_OK || sendCommand(0x11) != ESP_OK) return false;
  delay(60);
  if (sendCommand(0x29) != ESP_OK) return false;
  // The factory app calls panel_disp_on_off(true) after its custom table,
  // resulting in a second display-on command.
  if (sendCommand(0x29) != ESP_OK) return false;
  digitalWrite(LCD_BL, HIGH);
  return true;
}

void fillPanelSolid(uint16_t color) {
  if (!lcdIo || !dmaStrip || !frameFree) return;
  for (size_t i = 0; i < static_cast<size_t>(LCD_TRANSFER_W) * STRIP_ROWS; ++i) dmaStrip[i] = color;
  const uint8_t columns[] = {0x00,0x06,0x01,0xdd};
  if (xSemaphoreTake(frameFree, pdMS_TO_TICKS(250)) != pdTRUE) return;
  for (int y = 0; y < H; y += STRIP_ROWS) {
    const int count = min(STRIP_ROWS, H - y);
    const int last = y + count - 1;
    const uint8_t rows[] = {
      static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
      static_cast<uint8_t>(last >> 8), static_cast<uint8_t>(last)
    };
    esp_err_t err = sendCommand(0x2a, columns, sizeof(columns));
    if (err == ESP_OK) err = sendCommand(0x2b, rows, sizeof(rows));
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_color(
      lcdIo, qspiCommand(0x32,0x2c), dmaStrip,
      static_cast<size_t>(LCD_TRANSFER_W) * count * sizeof(uint16_t));
    if (err != ESP_OK) { xSemaphoreGive(frameFree); return; }
    if (y + count < H && xSemaphoreTake(frameFree, pdMS_TO_TICKS(250)) != pdTRUE) return;
  }
}

bool readTouchRegisters(uint8_t reg, uint8_t* data, size_t length) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(TOUCH_ADDR), static_cast<int>(length)) != length) return false;
  for (size_t i=0;i<length;++i) data[i]=Wire.read();
  return true;
}

bool initTouch() {
  pinMode(TOUCH_INT, INPUT_PULLUP); pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW); delay(12); digitalWrite(TOUCH_RST, HIGH); delay(50);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  uint8_t chipId = 0;
  return readTouchRegisters(0xa7, &chipId, 1);
}
} // namespace

void boardUiBegin() {
  boardStartedAt = millis();
  frame = static_cast<uint16_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame) frame = static_cast<uint16_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_8BIT));
  transitionOld = static_cast<uint16_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  transitionNew = static_cast<uint16_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  dmaStrip = static_cast<uint16_t*>(heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  frameFree = xSemaphoreCreateBinary();
  if (!dmaStrip || !frameFree) {
    Serial.printf("display allocation failed frame=%p strip=%p sem=%p\n", frame, dmaStrip, frameFree);
    ESP_LOGE("knob-display", "allocation failed frame=%p strip=%p sem=%p", frame, dmaStrip, frameFree);
    return;
  }
  xSemaphoreGive(frameFree);
  displayReady = initDisplay();
  Serial.printf("display init: %s\n", displayReady ? "ok" : "failed");
  ESP_LOGE("knob-display", "display init: %s", displayReady ? "ok" : "failed");
  if (displayReady && !frame) {
    // Low-memory diagnostic/fallback: prove the panel path without a full
    // framebuffer.  A normal OPI-PSRAM build never takes this branch.
    fillPanelSolid(0xffff);
  }
  touchReady = initTouch();
  Serial.printf("touch init: %s\n", touchReady ? "ok" : "failed");
  ESP_LOGE("knob-display", "touch init: %s", touchReady ? "ok" : "failed");
}

void boardUiRender(const KnobUiState& state) {
  static bool firstFrameReported = false;
  static bool wasBooting = true;
  static bool pageKnown = false;
  static KnobPage previousPage = KnobPage::Navigation;
  if (!displayReady || !frame) return;
  const uint32_t now = millis();
  if (displayAwakeRequested != displayAwake) {
    if (xSemaphoreTake(frameFree, pdMS_TO_TICKS(300)) != pdTRUE) return;
    if (displayAwakeRequested) {
      // Wake the SH8601 while leaving the ESP32 and BLE HID fully running.
      sendCommand(0x11);
      delay(120);
      sendCommand(0x29);
      displayAwake = true;
      lastFrameAt = 0;
    } else {
      sendCommand(0x28);
      delay(20);
      sendCommand(0x10);
      displayAwake = false;
    }
    xSemaphoreGive(frameFree);
  }
  if (!displayAwake) return;
  if (now - lastFrameAt < 33) return;
  lastFrameAt = now;
  if (xSemaphoreTake(frameFree, pdMS_TO_TICKS(250)) != pdTRUE) {
    Serial.println("display frame semaphore timeout");
    return;
  }

  const bool bootEnded = wasBooting && !state.booting;
  const bool pageChanged = !state.booting && pageKnown && state.page != previousPage;
  if ((bootEnded || pageChanged) && transitionOld && transitionNew) {
    memcpy(transitionOld, frame, FRAME_BYTES);
    transitionStartedAt = now;
    transitionFromBoot = bootEnded;
    transitionDirection = state.pageDirection == 0 ? 1 : state.pageDirection;
  }

  if (state.booting) drawBoot(now);
  else {
    clear(UiV12::BLACK);
    switch (state.page) {
      case KnobPage::Navigation: drawNavigation(state); break;
      case KnobPage::Touchpad: drawTouchpad(state); break;
      case KnobPage::Media: drawMedia(state); break;
      case KnobPage::System: drawSystem(state); break;
    }
  }

  if (!state.booting && transitionStartedAt && transitionOld && transitionNew) {
    const uint32_t duration = transitionFromBoot ? 400 : UiV12::PAGE_TRANSITION_MS;
    const float progress = min(1.0f, (now - transitionStartedAt) / static_cast<float>(duration));
    memcpy(transitionNew, frame, FRAME_BYTES);
    composeTransition(progress);
    if (progress >= 1.0f) transitionStartedAt = 0;
  }
  wasBooting = state.booting;
  if (!state.booting) { previousPage = state.page; pageKnown = true; }

  const int xEnd = LCD_X_OFFSET + LCD_TRANSFER_W - 1;
  const uint8_t range[] = {
    static_cast<uint8_t>(LCD_X_OFFSET >> 8), static_cast<uint8_t>(LCD_X_OFFSET & 0xff),
    static_cast<uint8_t>(xEnd >> 8), static_cast<uint8_t>(xEnd & 0xff)
  };
  for (int y = 0; y < H; y += STRIP_ROWS) {
    const int rowsToSend = min(STRIP_ROWS, H - y);
    const size_t bytesToSend = static_cast<size_t>(LCD_TRANSFER_W) * rowsToSend * sizeof(uint16_t);
    memcpy(dmaStrip, frame + static_cast<size_t>(y) * W, bytesToSend);
    const int lastY = y + rowsToSend - 1;
    const uint8_t rows[] = {
      static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xff),
      static_cast<uint8_t>(lastY >> 8), static_cast<uint8_t>(lastY & 0xff)
    };
    esp_err_t err = sendCommand(0x2a, range, sizeof(range));
    if (err == ESP_OK) err = sendCommand(0x2b, rows, sizeof(rows));
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_color(lcdIo, qspiCommand(0x32,0x2c), dmaStrip, bytesToSend);
    if (err != ESP_OK) {
      Serial.printf("display transfer failed at y=%d: %s\n", y, esp_err_to_name(err));
      xSemaphoreGive(frameFree);
      return;
    }
    if (y + rowsToSend < H && xSemaphoreTake(frameFree, pdMS_TO_TICKS(250)) != pdTRUE) {
      Serial.printf("display callback timeout at y=%d\n", y);
      return;
    }
  }
  if (!firstFrameReported) {
    firstFrameReported = true;
    Serial.println("display first frame queued successfully");
  }
}

void boardUiSetAwake(bool awake) {
  displayAwakeRequested = awake;
}

bool boardUiIsAwake() {
  return displayAwakeRequested;
}

bool boardReadTouch(uint16_t& x, uint16_t& y, bool& pressed) {
  if (!touchReady) { pressed = false; return false; }
  uint8_t data[5]{};
  if (!readTouchRegisters(0x02, data, sizeof(data))) { pressed = false; return false; }
  pressed = (data[0] & 0x0f) != 0;
  if (pressed) {
    x = constrain(static_cast<uint16_t>(((data[1] & 0x0f) << 8) | data[2]), 0, W - 1);
    y = constrain(static_cast<uint16_t>(((data[3] & 0x0f) << 8) | data[4]), 0, H - 1);
  }
  return true;
}
