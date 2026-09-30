using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CastMirror.Services
{
    /// <summary>
    /// Translates the literal text in a window's XAML in one pass after it loads.
    /// The XAML keeps its English, so nothing is bound and nothing breaks when a
    /// translation is missing. Text that comes from the view model is translated in
    /// code with <see cref="Localizer.T"/> instead.
    /// </summary>
    public static class UiTranslator
    {
        public static void Apply(Window window)
        {
            if (Localizer.Language == "en") return;
            window.Title = Localizer.T(window.Title);
            if (window.Content is DependencyObject root) Walk(root);
        }

        /// <summary>Translates one element and everything under it (used for flyouts, which sit outside the window tree).</summary>
        public static void Apply(DependencyObject root)
        {
            if (Localizer.Language == "en") return;
            Walk(root);
        }

        private static void Walk(DependencyObject node)
        {
            switch (node)
            {
                case TextBlock text:
                    if (string.IsNullOrEmpty(text.Text) == false) text.Text = Localizer.T(text.Text);
                    break;
                case TextBox box:
                    box.PlaceholderText = Localizer.T(box.PlaceholderText);
                    break;
                case ToggleSwitch toggle:
                    if (toggle.Header is string header) toggle.Header = Localizer.T(header);
                    toggle.OnContent = TranslateObject(toggle.OnContent);
                    toggle.OffContent = TranslateObject(toggle.OffContent);
                    break;
                case Expander expander:
                    if (expander.Header is string expanderHeader) expander.Header = Localizer.T(expanderHeader);
                    break;
                case ContentControl control when control.Content is string content:
                    control.Content = Localizer.T(content);
                    break;
            }

            if (node is FrameworkElement element)
            {
                string name = AutomationProperties.GetName(element);
                if (!string.IsNullOrEmpty(name)) AutomationProperties.SetName(element, Localizer.T(name));
                if (ToolTipService.GetToolTip(element) is string tip)
                {
                    ToolTipService.SetToolTip(element, Localizer.T(tip));
                }
            }

            // Logical children: content, panel children and single-child hosts. Walking
            // these instead of the visual tree reaches collapsed Expander bodies too.
            switch (node)
            {
                case Panel panel:
                    foreach (UIElement child in panel.Children) Walk(child);
                    break;
                case Border border when border.Child != null:
                    Walk(border.Child);
                    break;
                case ScrollViewer viewer when viewer.Content is DependencyObject scrolled:
                    Walk(scrolled);
                    break;
                case Expander expanderBody when expanderBody.Content is DependencyObject body:
                    Walk(body);
                    break;
                case ContentControl host when host.Content is DependencyObject hosted:
                    Walk(hosted);
                    break;
                case ItemsControl items:
                    foreach (object item in items.Items)
                    {
                        if (item is DependencyObject dependencyItem) Walk(dependencyItem);
                    }
                    break;
            }
        }

        private static object? TranslateObject(object? value) =>
            value is string s ? Localizer.T(s) : value;
    }
}
