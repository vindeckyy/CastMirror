#ifndef CASTMIRROR_GUI_SPARKLINE_H_
#define CASTMIRROR_GUI_SPARKLINE_H_

#include <gtk/gtk.h>
#include <vector>

namespace castcore::gui {

class Sparkline {
 public:
  Sparkline(int max_points = 40, float min_val = 0.0f, float max_val = 100.0f,
            double r = 0.0, double g = 0.82, double b = 1.0);
  ~Sparkline();

  Sparkline(const Sparkline&) = delete;
  Sparkline& operator=(const Sparkline&) = delete;

  GtkWidget* GetWidget() const { return drawing_area_; }

  void PushValue(float val);
  void SetRange(float min_val, float max_val);
  void SetColor(double r, double g, double b);
  void Reset();

 private:
  static void DrawCallback(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data);
  void Draw(cairo_t* cr, int width, int height);

  GtkWidget* drawing_area_ = nullptr;
  std::vector<float> values_;
  int max_points_ = 40;
  float min_val_ = 0.0f;
  float max_val_ = 100.0f;
  double color_r_ = 0.0;
  double color_g_ = 0.82;
  double color_b_ = 1.0;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_SPARKLINE_H_
