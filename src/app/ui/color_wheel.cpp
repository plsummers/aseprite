// Aseprite
// Copyright (C) 2020-2022  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/color_wheel.h"

#include "app/color_utils.h"
#include "app/pref/preferences.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "app/util/shader_helpers.h"
#include "base/pi.h"
#include "os/surface.h"
#include "ui/graphics.h"
#include "ui/menu.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/resize_event.h"
#include "ui/size_hint_event.h"
#include "ui/system.h"

namespace app {

using namespace app::skin;
using namespace gfx;
using namespace ui;

static struct {
  int n;
  int hues[4];
  int sats[4];
} harmonies[] = {
  { 1, { 0,   0,   0,   0 }, { 100,   0,   0,   0 } }, // NONE
  { 2, { 0, 180,   0,   0 }, { 100, 100,   0,   0 } }, // COMPLEMENTARY
  { 2, { 0,   0,   0,   0 }, { 100,  50,   0,   0 } }, // MONOCHROMATIC
  { 3, { 0,  30, 330,   0 }, { 100, 100, 100,   0 } }, // ANALOGOUS
  { 3, { 0, 150, 210,   0 }, { 100, 100, 100,   0 } }, // SPLIT
  { 3, { 0, 120, 240,   0 }, { 100, 100, 100,   0 } }, // TRIADIC
  { 4, { 0, 120, 180, 300 }, { 100, 100, 100, 100 } }, // TETRADIC
  { 4, { 0,  90, 180, 270 }, { 100, 100, 100, 100 } }, // SQUARE
};

ColorWheel::ColorWheel()
  : m_discrete(Preferences::instance().colorBar.discreteWheel())
  , m_colorModel((ColorModel)Preferences::instance().colorBar.wheelModel())
  , m_harmony((Harmony)Preferences::instance().colorBar.harmony())
  , m_options("")
  , m_harmonyPicked(false)
{
  m_options.Click.connect([this]{ onOptions(); });
  addChild(&m_options);

  InitTheme.connect(
    [this]{
      auto theme = skin::SkinTheme::get(this);
      m_options.setStyle(theme->styles.colorWheelOptions());
      m_bgColor = theme->colors.editorFace();
    });
  initTheme();
}

#if SK_ENABLE_SKSL

const char* ColorWheel::getMainAreaShader()
{
  // TODO create one shader for each wheel mode (RGB, RYB, normal)
  if (m_mainShader.empty()) {
    m_mainShader += "uniform half3 iRes;"
                    "uniform half4 iHsv;"
                    "uniform half4 iBack;"
                    "uniform int iDiscrete;"
                    "uniform int iMode;";
    m_mainShader += kRGB_to_HSV_sksl;
    m_mainShader += kHSV_to_RGB_sksl;
    m_mainShader += R"(
const half PI = 3.1415;

half rybhue_to_rgbhue(half h) {
 if (h >= 0 && h < 120) return h / 2;      // from red to yellow
 else if (h < 180) return (h-60.0);        // from yellow to green
 else if (h < 240) return 120 + 2*(h-180); // from green to blue
 else return h;                            // from blue to red (same hue)
}

half4 main(vec2 fragcoord) {
 vec2 res = vec2(min(iRes.x, iRes.y), min(iRes.x, iRes.y));
 vec2 d = (fragcoord.xy-iRes.xy/2) / res.xy;
 half r = length(d);

 if (r <= 0.5) {
  half a = atan(-d.y, d.x);
  half hue = (floor(180.0 * a / PI)
             + 180            // To avoid [-180,0) range
             + 180 + 30       // To locate green at 12 o'clock
             );

  hue = mod(hue, 360);   // To leave hue in [0,360) range
  if (iDiscrete != 0) {
   hue += 15.0;
   hue = floor(hue / 30.0);
   hue *= 30.0;
  }
  if (iMode == 1) { // RYB color wheel
   hue = rybhue_to_rgbhue(hue);
  }
  hue /= 360.0;

  if (iMode == 2) { // Normal map mode
   float di = 0.5 * r / 0.5;
   half3 rgb = half3(0.5+di*cos(a), 0.5+di*sin(a), 1.0-di);
   return half4(
    clamp(rgb.x, 0, 1),
    clamp(rgb.y, 0, 1),
    clamp(rgb.z, 0.5, 1), 1);
  }

  half sat = r / 0.5;
  if (iDiscrete != 0) {
   sat *= 120.0;
   sat = floor(sat / 20.0);
   sat *= 20.0;
   sat /= 100.0;
   sat = clamp(sat, 0.0, 1.0);
  }
  return hsv_to_rgb(vec3(hue, sat, iHsv.w > 0 ? iHsv.z: 1.0)).rgb1;
 }
 else {
  if (iMode == 2) // Normal map mode
   return half4(0.5, 0.5, 1, 1);
  return iBack;
 }
}
)";
  }
  return m_mainShader.c_str();
}

const char* ColorWheel::getBottomBarShader()
{
  if (m_bottomShader.empty()) {
    m_bottomShader += "uniform half3 iRes;"
                      "uniform half4 iHsv;";
    m_bottomShader += kHSV_to_RGB_sksl;
    // TODO should we display the hue bar with the current sat/value?
    m_bottomShader += R"(
half4 main(vec2 fragcoord) {
 half v = (fragcoord.x / iRes.x);
 return hsv_to_rgb(half3(iHsv.x, iHsv.y, v)).rgb1;
}
)";
  }
  return m_bottomShader.c_str();
}

void ColorWheel::setShaderParams(SkRuntimeShaderBuilder& builder, bool main)
{
  builder.uniform("iHsv") = appColorHsv_to_SkV4(m_color);
  if (main) {
    builder.uniform("iBack") = gfxColor_to_SkV4(m_bgColor);
    builder.uniform("iDiscrete") = (m_discrete ? 1: 0);
    builder.uniform("iMode") = int(m_colorModel);
  }
}

#endif // SK_ENABLE_SKSL

app::Color ColorWheel::getMainAreaColor(const int _u, const int umax,
                                        const int _v, const int vmax)
{
  m_harmonyPicked = false;

  int u = _u - umax/2;
  int v = _v - vmax/2;

  // Pick harmonies
  if (m_color.getAlpha() > 0) {
    const gfx::Point pos(_u, _v);
    int n = getHarmonies();
    int boxsize = std::min(umax/10, vmax/10);

    for (int i=0; i<n; ++i) {
      app::Color color = getColorInHarmony(i);

      if (gfx::Rect(umax-(n-i)*boxsize,
                    vmax-boxsize,
                    boxsize, boxsize).contains(pos)) {
        m_harmonyPicked = true;

        color = app::Color::fromHsv(convertHueAngle(color.getHsvHue(), 1),
                                    color.getHsvSaturation(),
                                    color.getHsvValue(),
                                    m_color.getAlpha());
        return color;
      }
    }
  }

  double d = std::sqrt(u*u + v*v);

  // When we click the main area we can limit the distance to the
  // wheel radius to pick colors even outside the wheel radius.
  if (hasCaptureInMainArea() && d > m_wheelRadius)
    d = m_wheelRadius;

  if (m_colorModel == ColorModel::NORMAL_MAP) {
    double a = std::atan2(-v, u);
    int di = int(128.0 * d / m_wheelRadius);

    if (m_discrete) {
      int ai = (int(180.0 * a / PI) + 360);
      ai += 15;
      ai /= 30;
      ai *= 30;
      a = PI * ai / 180.0;

      di /= 32;
      di *= 32;
    }

    int r = 128 + di*std::cos(a);
    int g = 128 + di*std::sin(a);
    int b = 255 - di;
    if (d <= m_wheelRadius) {
      return app::Color::fromRgb(
        std::clamp(r, 0, 255),
        std::clamp(g, 0, 255),
        std::clamp(b, 128, 255));
    }
    else {
      return app::Color::fromRgb(128, 128, 255);
    }
  }

  // Pick from the wheel
  if (d <= m_wheelRadius) {
    double a = std::atan2(-v, u);

    int hue = (int(180.0 * a / PI)
               + 180            // To avoid [-180,0) range
               + 180 + 30       // To locate green at 12 o'clock
               );
    if (m_discrete) {
      hue += 15;
      hue /= 30;
      hue *= 30;
    }
    hue %= 360;                 // To leave hue in [0,360) range
    hue = convertHueAngle(hue, 1);

    int sat;
    if (m_discrete) {
      sat = int(120.0 * d / m_wheelRadius);
      sat /= 20;
      sat *= 20;
    }
    else {
      sat = int(100.0 * d / m_wheelRadius);
    }

    return app::Color::fromHsv(
      std::clamp(hue, 0, 360),
      std::clamp(sat / 100.0, 0.0, 1.0),
      (m_color.getType() != Color::MaskType ? m_color.getHsvValue(): 1.0),
      getCurrentAlphaForNewColor());
  }

  return app::Color::fromMask();
}

app::Color ColorWheel::getBottomBarColor(const int u, const int umax)
{
  double val = double(u) / double(umax);
  return app::Color::fromHsv(
    m_color.getHsvHue(),
    m_color.getHsvSaturation(),
    std::clamp(val, 0.0, 1.0),
    getCurrentAlphaForNewColor());
}

void ColorWheel::onPaintMainArea(ui::Graphics* g, const gfx::Rect& rc)
{
  bool oldHarmonyPicked = m_harmonyPicked;

  double r = std::max(1.0, std::min(rc.w, rc.h) / 2.0);
  m_wheelRadius = r-0.1;
  m_wheelBounds = gfx::Rect(rc.x+rc.w/2-r,
                            rc.y+rc.h/2-r,
                            r*2, r*2);

  if (m_color.getAlpha() > 0) {
    if (m_colorModel == ColorModel::NORMAL_MAP) {
      double angle = std::atan2(m_color.getGreen()-128,
                                m_color.getRed()-128);
      double dist = (255-m_color.getBlue()) / 128.0;
      dist = std::clamp(dist, 0.0, 1.0);

      gfx::Point pos =
        m_wheelBounds.center() +
        gfx::Point(int(+std::cos(angle)*double(m_wheelRadius)*dist),
                   int(-std::sin(angle)*double(m_wheelRadius)*dist));
      paintColorIndicator(g, pos, true);
    }
    else {
      int n = getHarmonies();
      int boxsize = std::min(rc.w/10, rc.h/10);

      for (int i=0; i<n; ++i) {
        app::Color color = getColorInHarmony(i);
        double angle = color.getHsvHue()-30.0;
        double dist = color.getHsvSaturation();

        color = app::Color::fromHsv(convertHueAngle(color.getHsvHue(), 1),
                                    color.getHsvSaturation(),
                                    color.getHsvValue());

        gfx::Point pos =
          m_wheelBounds.center() +
          gfx::Point(int(+std::cos(PI*angle/180.0)*double(m_wheelRadius)*dist),
                     int(-std::sin(PI*angle/180.0)*double(m_wheelRadius)*dist));

        paintColorIndicator(g, pos, color.getHsvValue() < 0.5);

        g->fillRect(gfx::rgba(color.getRed(),
                              color.getGreen(),
                              color.getBlue(), 255),
                    gfx::Rect(rc.x+rc.w-(n-i)*boxsize,
                              rc.y+rc.h-boxsize,
                              boxsize, boxsize));
      }
    }
  }

  m_harmonyPicked = oldHarmonyPicked;
}

void ColorWheel::onPaintBottomBar(ui::Graphics* g, const gfx::Rect& rc)
{
  if (m_color.getType() != app::Color::MaskType) {
    double val = m_color.getHsvValue();
    gfx::Point pos(rc.x + int(double(rc.w) * val),
                   rc.y + rc.h/2);
    paintColorIndicator(g, pos, val < 0.5);
  }
}

void ColorWheel::onPaintSurfaceInBgThread(os::Surface* s,
                                          const gfx::Rect& main,
                                          const gfx::Rect& bottom,
                                          const gfx::Rect& alpha,
                                          bool& stop)
{
  if (m_paintFlags & MainAreaFlag) {
    int umax = std::max(1, main.w-1);
    int vmax = std::max(1, main.h-1);

    for (int y=0; y<main.h && !stop; ++y) {
      for (int x=0; x<main.w && !stop; ++x) {
        app::Color appColor =
          getMainAreaColor(x, umax,
                           y, vmax);

        gfx::Color color;
        if (appColor.getType() != app::Color::MaskType) {
          appColor.setAlpha(255);
          color = color_utils::color_for_ui(appColor);
        }
        else {
          color = m_bgColor;
        }

        s->putPixel(color, main.x+x, main.y+y);
      }
    }
    if (stop)
      return;
    m_paintFlags ^= MainAreaFlag;
  }

  if (m_paintFlags & BottomBarFlag) {
    double hue = m_color.getHsvHue();
    double sat = m_color.getHsvSaturation();
    os::Paint paint;
    for (int x=0; x<bottom.w && !stop; ++x) {
      paint.color(
        color_utils::color_for_ui(
          app::Color::fromHsv(hue, sat, double(x) / double(bottom.w))));

      s->drawRect(gfx::Rect(bottom.x+x, bottom.y, 1, bottom.h), paint);
    }
    if (stop)
      return;
    m_paintFlags ^= BottomBarFlag;
  }

  // Paint alpha bar
  ColorSelector::onPaintSurfaceInBgThread(s, main, bottom, alpha, stop);
}

int ColorWheel::onNeedsSurfaceRepaint(const app::Color& newColor)
{
  return
    // Only if the saturation changes we have to redraw the main surface.
    (m_colorModel != ColorModel::NORMAL_MAP &&
     cs_double_diff(m_color.getHsvValue(), newColor.getHsvValue()) ? MainAreaFlag: 0) |
    (cs_double_diff(m_color.getHsvHue(), newColor.getHsvHue()) ||
     cs_double_diff(m_color.getHsvSaturation(), newColor.getHsvSaturation()) ? BottomBarFlag: 0) |
    ColorSelector::onNeedsSurfaceRepaint(newColor);
}

void ColorWheel::setDiscrete(bool state)
{
  if (m_discrete != state)
    m_paintFlags = AllAreasFlag;

  m_discrete = state;
  Preferences::instance().colorBar.discreteWheel(m_discrete);

  invalidate();
}

void ColorWheel::setColorModel(ColorModel colorModel)
{
  m_colorModel = colorModel;
  Preferences::instance().colorBar.wheelModel((int)m_colorModel);

  invalidate();
}

void ColorWheel::setHarmony(Harmony harmony)
{
  m_harmony = harmony;
  Preferences::instance().colorBar.harmony((int)m_harmony);

  invalidate();
}

int ColorWheel::getHarmonies() const
{
  int i = std::clamp((int)m_harmony, 0, (int)Harmony::LAST);
  return harmonies[i].n;
}

app::Color ColorWheel::getColorInHarmony(int j) const
{
  int i = std::clamp((int)m_harmony, 0, (int)Harmony::LAST);
  j = std::clamp(j, 0, harmonies[i].n-1);
  double hue = convertHueAngle(m_color.getHsvHue(), -1) + harmonies[i].hues[j];
  double sat = m_color.getHsvSaturation() * harmonies[i].sats[j] / 100.0;
  return app::Color::fromHsv(std::fmod(hue, 360),
                             std::clamp(sat, 0.0, 1.0),
                             m_color.getHsvValue());
}

void ColorWheel::onResize(ui::ResizeEvent& ev)
{
  ColorSelector::onResize(ev);

  gfx::Rect rc = clientChildrenBounds();
  gfx::Size prefSize = m_options.sizeHint();
  rc = childrenBounds();
  rc.x += rc.w-prefSize.w;
  rc.w = prefSize.w;
  rc.h = prefSize.h;
  m_options.setBounds(rc);
}

void ColorWheel::onOptions()
{
  Menu menu;
  MenuItem discrete("Discrete");
  MenuItem none("Without Harmonies");
  MenuItem complementary("Complementary");
  MenuItem monochromatic("Monochromatic");
  MenuItem analogous("Analogous");
  MenuItem split("Split-Complementary");
  MenuItem triadic("Triadic");
  MenuItem tetradic("Tetradic");
  MenuItem square("Square");
  menu.addChild(&discrete);
  if (m_colorModel != ColorModel::NORMAL_MAP) {
    menu.addChild(new MenuSeparator);
    menu.addChild(&none);
    menu.addChild(&complementary);
    menu.addChild(&monochromatic);
    menu.addChild(&analogous);
    menu.addChild(&split);
    menu.addChild(&triadic);
    menu.addChild(&tetradic);
    menu.addChild(&square);
  }

  if (isDiscrete())
    discrete.setSelected(true);
  discrete.Click.connect([this]{ setDiscrete(!isDiscrete()); });

  if (m_colorModel != ColorModel::NORMAL_MAP) {
    switch (m_harmony) {
      case Harmony::NONE: none.setSelected(true); break;
      case Harmony::COMPLEMENTARY: complementary.setSelected(true); break;
      case Harmony::MONOCHROMATIC: monochromatic.setSelected(true); break;
      case Harmony::ANALOGOUS: analogous.setSelected(true); break;
      case Harmony::SPLIT: split.setSelected(true); break;
      case Harmony::TRIADIC: triadic.setSelected(true); break;
      case Harmony::TETRADIC: tetradic.setSelected(true); break;
      case Harmony::SQUARE: square.setSelected(true); break;
    }
    none.Click.connect([this]{ setHarmony(Harmony::NONE); });
    complementary.Click.connect([this]{ setHarmony(Harmony::COMPLEMENTARY); });
    monochromatic.Click.connect([this]{ setHarmony(Harmony::MONOCHROMATIC); });
    analogous.Click.connect([this]{ setHarmony(Harmony::ANALOGOUS); });
    split.Click.connect([this]{ setHarmony(Harmony::SPLIT); });
    triadic.Click.connect([this]{ setHarmony(Harmony::TRIADIC); });
    tetradic.Click.connect([this]{ setHarmony(Harmony::TETRADIC); });
    square.Click.connect([this]{ setHarmony(Harmony::SQUARE); });
  }

  gfx::Rect rc = m_options.bounds();
  menu.showPopup(gfx::Point(rc.x+rc.w, rc.y));
}

float ColorWheel::convertHueAngle(float h, int dir) const
{
  if (m_colorModel == ColorModel::RYB) {
    if (dir == 1) {
      // rybhue_to_rgbhue() maps:
      //   [0,120) -> [0,60)
      //   [120,180) -> [60,120)
      //   [180,240) -> [120,240)
      //   [240,360] -> [240,360]
      if (h >= 0 && h < 120) return h / 2;      // from red to yellow
      else if (h < 180) return (h-60);          // from yellow to green
      else if (h < 240) return 120 + 2*(h-180); // from green to blue
      else return h;                             // from blue to red (same hue)
    }
    else {
      // rgbhue_to_rybhue()
      //   [0,60) -> [0,120)
      //   [60,120) -> [120,180)
      //   [120,240) -> [180,240)
      //   [240,360] -> [240,360]
      if (h >= 0 && h < 60) return 2 * h;       // from red to yellow
      else if (h < 120) return 60 + h;          // from yellow to green
      else if (h < 240) return 180 + (h-120)/2; // from green to blue
      else return h;                            // from blue to red (same hue)
    }
  }
  return h;
}

} // namespace app
