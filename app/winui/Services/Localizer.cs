using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;

namespace CastMirror.Services
{
    /// <summary>
    /// Looks up a translation for an English UI string. English is the source text
    /// everywhere (in the XAML and in code), and a language table maps each English
    /// string to its translation. A string with no entry, or a language with no
    /// table, shows the English text, so a missing translation can never leave a
    /// blank control.
    ///
    /// Tables are JSON objects in <c>Strings/&lt;language&gt;.json</c>, embedded in the
    /// app. To add a language, add a file with the two-letter code (or code-REGION)
    /// and translate the values. The language follows the Windows display language;
    /// set CASTMIRROR_LANG to try another one.
    /// </summary>
    public static class Localizer
    {
        private static IReadOnlyDictionary<string, string> _table =
            new Dictionary<string, string>();

        /// <summary>The language currently in use ("en" when no table applies).</summary>
        public static string Language { get; private set; } = "en";

        /// <summary>
        /// Chooses the language from CASTMIRROR_LANG, else the Windows display
        /// language, and loads its table from the app's embedded resources.
        /// </summary>
        public static void Initialize(Assembly? assembly = null)
        {
            string requested = Environment.GetEnvironmentVariable("CASTMIRROR_LANG")
                               ?? CultureInfo.CurrentUICulture.Name;
            Use(requested, assembly ?? typeof(Localizer).Assembly);
        }

        /// <summary>Loads the best available table for a culture name such as "pt-BR" or "de".</summary>
        public static void Use(string culture, Assembly assembly)
        {
            foreach (string candidate in Candidates(culture))
            {
                IReadOnlyDictionary<string, string>? table = LoadTable(assembly, candidate);
                if (table != null)
                {
                    _table = table;
                    Language = candidate;
                    return;
                }
            }
            _table = new Dictionary<string, string>();
            Language = "en";
        }

        /// <summary>Installs a table directly (tests, and tools that build one in memory).</summary>
        public static void UseTable(string language, IReadOnlyDictionary<string, string> table)
        {
            _table = table;
            Language = language;
        }

        /// <summary>"pt-BR" tries "pt-BR" then "pt"; "de" tries "de".</summary>
        public static IEnumerable<string> Candidates(string culture)
        {
            if (string.IsNullOrWhiteSpace(culture)) yield break;
            string full = culture.Trim();
            if (full.StartsWith("en", StringComparison.OrdinalIgnoreCase)) yield break;
            yield return full;
            int dash = full.IndexOf('-');
            if (dash > 0) yield return full[..dash];
        }

        private static IReadOnlyDictionary<string, string>? LoadTable(Assembly assembly, string language)
        {
            string suffix = $"Strings.{language}.json";
            foreach (string name in assembly.GetManifestResourceNames())
            {
                if (!name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase)) continue;
                using Stream? stream = assembly.GetManifestResourceStream(name);
                if (stream == null) continue;
                try
                {
                    return ParseTable(stream);
                }
                catch (JsonException ex)
                {
                    LogService.Log($"[l10n] {name} is not valid JSON: {ex.Message}");
                }
            }
            return null;
        }

        public static IReadOnlyDictionary<string, string> ParseTable(Stream json)
        {
            var table = JsonSerializer.Deserialize<Dictionary<string, string>>(json)
                        ?? new Dictionary<string, string>();
            // An empty translation means "not translated yet"; fall back to English.
            foreach (string key in new List<string>(table.Keys))
            {
                if (string.IsNullOrWhiteSpace(table[key])) table.Remove(key);
            }
            return table;
        }

        /// <summary>The translation of <paramref name="english"/>, or the English text itself.</summary>
        public static string T(string english)
        {
            return _table.TryGetValue(english, out string? translated) ? translated : english;
        }

        /// <summary>Translates a template, then fills its {0}, {1} placeholders.</summary>
        public static string Format(string englishTemplate, params object?[] args)
        {
            string template = T(englishTemplate);
            try
            {
                return string.Format(CultureInfo.CurrentCulture, template, args);
            }
            catch (FormatException)
            {
                // A translation that lost or mangled a placeholder must not crash the UI.
                return string.Format(CultureInfo.CurrentCulture, englishTemplate, args);
            }
        }
    }
}
