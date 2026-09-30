using System;
using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace CastMirror.Controls
{
    /// <summary>
    /// Lightweight real-time sparkline: a fixed-capacity ring of samples rendered
    /// as a Polyline inside a Canvas. Deliberately dependency-free (no Win2D) so
    /// the Windows client keeps its current package set. Bind <see cref="Samples"/>
    /// to a double[] that the view model refreshes on each stats tick.
    /// </summary>
    public sealed class Sparkline : UserControl
    {
        private readonly Canvas _canvas = new();
        private readonly Polyline _line = new();
        private readonly double[] _samples;
        private int _count;

        public static readonly DependencyProperty SamplesProperty = DependencyProperty.Register(
            nameof(Samples),
            typeof(IEnumerable<double>),
            typeof(Sparkline),
            new PropertyMetadata(null, OnSamplesChanged));

        public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(
            nameof(Maximum),
            typeof(double),
            typeof(Sparkline),
            new PropertyMetadata(0.0, OnVisualChanged));

        /// <summary>Ring capacity, shared with the view model's history cap so
        /// neither side silently truncates the other's samples.</summary>
        public const int DefaultCapacity = 60;

        public Sparkline()
        {
            _samples = new double[DefaultCapacity];
            _line.StrokeThickness = 1.5;
            _line.StrokeLineJoin = PenLineJoin.Round;
            _line.Stroke = new SolidColorBrush(Color.FromArgb(255, 13, 110, 253));
            _canvas.Children.Add(_line);
            Content = _canvas;

            SizeChanged += (_, _) => Redraw();
        }

        /// <summary>Sample history, oldest first. Only the newest N are drawn.</summary>
        public IEnumerable<double>? Samples
        {
            get => (IEnumerable<double>?)GetValue(SamplesProperty);
            set => SetValue(SamplesProperty, value);
        }

        /// <summary>Upper bound used to scale the samples; 0 means auto-scale.</summary>
        public double Maximum
        {
            get => (double)GetValue(MaximumProperty);
            set => SetValue(MaximumProperty, value);
        }

        public Brush Stroke
        {
            get => _line.Stroke;
            set => _line.Stroke = value;
        }

        private static void OnSamplesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is Sparkline sparkline) sparkline.Load(e.NewValue as IEnumerable<double>);
        }

        private static void OnVisualChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is Sparkline sparkline) sparkline.Redraw();
        }

        private void Load(IEnumerable<double>? samples)
        {
            _count = 0;
            if (samples != null)
            {
                var list = samples as IList<double> ?? new List<double>(samples);
                int start = Math.Max(0, list.Count - _samples.Length);
                for (int i = start; i < list.Count; ++i)
                {
                    double value = list[i];
                    _samples[_count++] = double.IsFinite(value) ? value : 0;
                }
            }
            Redraw();
        }


        private void Redraw()
        {
            double width = _canvas.ActualWidth;
            double height = _canvas.ActualHeight;
            if (width <= 1 || height <= 1 || _count < 2)
            {
                _line.Points.Clear();
                return;
            }

            double max = Maximum;
            if (max <= 0)
            {
                max = 0;
                for (int i = 0; i < _count; ++i)
                {
                    if (_samples[i] > max) max = _samples[i];
                }
            }
            if (max <= 0) max = 1;

            var points = new PointCollection();
            for (int i = 0; i < _count; ++i)
            {
                double value = _samples[i];
                double x = width * i / (_count - 1);
                double y = height - (Math.Min(value, max) / max) * height;
                points.Add(new Point(x, y));
            }
            _line.Points = points;
        }
    }
}
