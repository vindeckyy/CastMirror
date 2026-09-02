#include "sparkline.h"
#include <algorithm>
#include <cmath>

namespace castcore::gui {

Sparkline::Sparkline(int max_points, float min_val, float max_val,
                     double r, double g, double b)
    : max_points_(std::max(5, max_points)),
      min_val_(min_val),
      max_val_(max_val),
      color_r_(r),
      color_g_(g),
      color_b_(b) {
  drawing_area_ = gtk_drawing_area_new();
  gtk_widget_set_size_request(drawing_area_, 80, 28);
  gtk_widget_set_hexpand(drawing_area_, TRUE);
  gtk_widget_set_valign(drawing_area_, GTK_ALIGN_END);
  gtk_widget_add_css_class(drawing_area_, "cm-sparkline");

  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(drawing_area_),
      DrawCallback,
      this,
      nullptr);
}

Sparkline::~Sparkline() = default;

void Sparkline::PushValue(float val) {
  values_.push_back(val);
  if (static_cast<int>(values_.size()) > max_points_) {
    values_.erase(values_.begin(), values_.begin() + (values_.size() - max_points_));
  }
  if (drawing_area_) {
    gtk_widget_queue_draw(drawing_area_);
  }
}

void Sparkline::SetRange(float min_val, float max_val) {
  min_val_ = min_val;
  max_val_ = std::max(min_val + 0.001f, max_val);
  if (drawing_area_) {
    gtk_widget_queue_draw(drawing_area_);
  }
}

void Sparkline::SetColor(double r, double g, double b) {
  color_r_ = r;
  color_g_ = g;
  color_b_ = b;
  if (drawing_area_) {
    gtk_widget_queue_draw(drawing_area_);
  }
}

void Sparkline::Reset() {
  values_.clear();
  if (drawing_area_) {
    gtk_widget_queue_draw(drawing_area_);
  }
}

void Sparkline::DrawCallback(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer user_data) {
  auto* self = static_cast<Sparkline*>(user_data);
  if (self) {
    self->Draw(cr, width, height);
  }
}

void Sparkline::Draw(cairo_t* cr, int width, int height) {
  if (width <= 0 || height <= 0) return;

  const double h = static_cast<double>(height);
  const double w = static_cast<double>(width);
  const double pad_y = 2.0;

  // If no data or single point, draw a subtle baseline
  if (values_.size() < 2) {
    cairo_set_source_rgba(cr, color_r_, color_g_, color_b_, 0.2);
    cairo_set_line_width(cr, 1.0);
    double dashes[] = {3.0, 3.0};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_move_to(cr, 0, h - pad_y);
    cairo_line_to(cr, w, h - pad_y);
    cairo_stroke(cr);
    return;
  }

  // Dynamic range if max_val <= min_val
  float effective_min = min_val_;
  float effective_max = max_val_;
  for (float v : values_) {
    if (v < effective_min) effective_min = v;
    if (v > effective_max) effective_max = v;
  }
  float span = std::max(0.001f, effective_max - effective_min);

  size_t n = values_.size();
  double dx = (n > 1) ? (w / static_cast<double>(max_points_ - 1)) : w;
  double start_x = w - static_cast<double>(n - 1) * dx;
  if (start_x < 0.0) start_x = 0.0;

  auto compute_y = [&](float val) -> double {
    double norm = static_cast<double>(std::clamp((val - effective_min) / span, 0.0f, 1.0f));
    return (h - pad_y) - norm * (h - 2.0 * pad_y);
  };

  // Build path
  cairo_new_path(cr);
  double first_x = start_x;
  double first_y = compute_y(values_[0]);
  cairo_move_to(cr, first_x, first_y);

  for (size_t i = 1; i < n; ++i) {
    double x = start_x + static_cast<double>(i) * dx;
    double y = compute_y(values_[i]);
    cairo_line_to(cr, x, y);
  }

  double last_x = start_x + static_cast<double>(n - 1) * dx;
  double last_y = compute_y(values_.back());

  // Copy path for stroke
  cairo_path_t* line_path = cairo_copy_path(cr);

  // Fill gradient underneath
  cairo_line_to(cr, last_x, h);
  cairo_line_to(cr, first_x, h);
  cairo_close_path(cr);

  cairo_pattern_t* grad = cairo_pattern_create_linear(0, 0, 0, h);
  cairo_pattern_add_color_stop_rgba(grad, 0.0, color_r_, color_g_, color_b_, 0.35);
  cairo_pattern_add_color_stop_rgba(grad, 1.0, color_r_, color_g_, color_b_, 0.02);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // Stroke top line
  cairo_append_path(cr, line_path);
  cairo_path_destroy(line_path);
  cairo_set_source_rgba(cr, color_r_, color_g_, color_b_, 0.95);
  cairo_set_line_width(cr, 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);

  // Draw dot at the end
  cairo_arc(cr, last_x, last_y, 3.0, 0, 2.0 * M_PI);
  cairo_set_source_rgba(cr, color_r_, color_g_, color_b_, 1.0);
  cairo_fill(cr);
}

}  // namespace castcore::gui
