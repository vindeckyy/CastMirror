using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using CastMirror.Services;
using Xunit;

namespace CastMirror.Tests
{
    public class LocalizerTests
    {
        private static readonly IReadOnlyDictionary<string, string> Table = new Dictionary<string, string>
        {
            ["Settings"] = "Ajustes",
            ["Connecting to {0}..."] = "Conectando con {0}...",
            ["Broken {0}"] = "Roto {1}",   // a translation that lost its placeholder
        };

        public LocalizerTests() => Localizer.UseTable("es", Table);

        [Fact]
        public void KnownStringIsTranslatedAndUnknownFallsBackToEnglish()
        {
            Assert.Equal("Ajustes", Localizer.T("Settings"));
            Assert.Equal("Untranslated text", Localizer.T("Untranslated text"));
        }

        [Fact]
        public void FormatFillsPlaceholdersInTheTranslation()
        {
            Assert.Equal("Conectando con Sala...", Localizer.Format("Connecting to {0}...", "Sala"));
        }

        [Fact]
        public void FormatFallsBackToEnglishWhenATranslationBreaksItsPlaceholders()
        {
            Assert.Equal("Broken x", Localizer.Format("Broken {0}", "x"));
        }

        [Theory]
        [InlineData("pt-BR", new[] { "pt-BR", "pt" })]
        [InlineData("de", new[] { "de" })]
        [InlineData("en-US", new string[0])]
        [InlineData("", new string[0])]
        public void CandidatesTryTheRegionThenTheLanguage(string culture, string[] expected)
        {
            Assert.Equal(expected, Localizer.Candidates(culture).ToArray());
        }

        [Fact]
        public void EmptyTranslationsCountAsMissing()
        {
            using var stream = new MemoryStream(Encoding.UTF8.GetBytes("{\"A\":\"a\",\"B\":\"\",\"C\":\"  \"}"));
            var table = Localizer.ParseTable(stream);
            Assert.Single(table);
            Assert.Equal("a", table["A"]);
        }
    }

    /// <summary>
    /// Keeps the tables honest against the real UI: nothing is left untranslated in
    /// the XAML, and no entry is a typo that can never match.
    /// </summary>
    public class TranslationCoverageTests
    {
        private static string Root
        {
            get
            {
                string? dir = AppContext.BaseDirectory;
                while (dir != null && !File.Exists(Path.Combine(dir, "app", "winui", "CastMirrorApp.csproj"))) dir = Path.GetDirectoryName(dir);
                return dir ?? throw new InvalidOperationException("repository root not found");
            }
        }

        private static readonly string[] Languages = { "es", "de", "fr" };

        private static Dictionary<string, string> Load(string language) =>
            JsonSerializer.Deserialize<Dictionary<string, string>>(
                File.ReadAllText(Path.Combine(Root, "app", "winui", "Strings", $"{language}.json"), Encoding.UTF8))!;

        private static HashSet<string> XamlStrings()
        {
            var found = new HashSet<string>();
            var attribute = new Regex(
                "\\s(Content|Text|Header|OnContent|OffContent|PlaceholderText|Title|ToolTipService\\.ToolTip|AutomationProperties\\.Name)=\"([^\"{][^\"]*)\"");
            foreach (string file in new[] { "MainWindow.xaml", "SettingsWindow.xaml", "LogsWindow.xaml" })
            {
                string xaml = File.ReadAllText(Path.Combine(Root, "app", "winui", file), Encoding.UTF8);
                foreach (Match m in attribute.Matches(xaml))
                {
                    string value = System.Net.WebUtility.HtmlDecode(m.Groups[2].Value);
                    if (value.StartsWith("&#x") || value.Length < 2) continue;
                    found.Add(value);
                }
            }
            return found;
        }

        // All C# source joined into one string with adjacent literals merged, so a key
        // that the code builds from "part one " + "part two" is still findable.
        private static string CodeText()
        {
            var text = new StringBuilder();
            foreach (string file in Directory.GetFiles(Path.Combine(Root, "app", "winui"), "*.cs", SearchOption.AllDirectories))
            {
                if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}") ||
                    file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}")) continue;
                text.AppendLine(File.ReadAllText(file, Encoding.UTF8));
            }
            // Health hints are English sentences produced by the engine and translated in the client.
            text.AppendLine(File.ReadAllText(Path.Combine(Root, "core", "src", "cast_session.cc"), Encoding.UTF8));
            string joined = Regex.Replace(text.ToString(), "\"\\s*\\+\\s*\\r?\\n?\\s*\\$?\"", string.Empty);
            return joined.Replace("\\n", "\n").Replace("\\\"", "\"");
        }

        [Fact]
        public void EveryXamlStringIsTranslatedInEveryLanguage()
        {
            var xaml = XamlStrings();
            Assert.NotEmpty(xaml);
            foreach (string language in Languages)
            {
                var table = Load(language);
                var missing = xaml.Where(s => !table.ContainsKey(s)).OrderBy(s => s).ToList();
                Assert.True(missing.Count == 0, $"{language}.json is missing: {string.Join(" | ", missing)}");
            }
        }

        [Fact]
        public void EveryKeyMatchesRealTextInTheXamlOrTheCode()
        {
            var xaml = XamlStrings();
            string code = CodeText();
            foreach (string language in Languages)
            {
                var orphans = Load(language).Keys
                    .Where(k => !xaml.Contains(k) && !code.Contains(k))
                    .OrderBy(k => k).ToList();
                Assert.True(orphans.Count == 0, $"{language}.json has keys that match nothing: {string.Join(" | ", orphans)}");
            }
        }

        [Fact]
        public void EveryLanguageHasTheSameKeysAndKeepsItsPlaceholders()
        {
            var english = Load("es").Keys.OrderBy(k => k).ToList();
            foreach (string language in Languages)
            {
                var table = Load(language);
                Assert.Equal(english, table.Keys.OrderBy(k => k).ToList());
                foreach (var (key, value) in table)
                {
                    var wanted = Regex.Matches(key, "\\{\\d+\\}").Select(m => m.Value).OrderBy(v => v);
                    var got = Regex.Matches(value, "\\{\\d+\\}").Select(m => m.Value).OrderBy(v => v);
                    Assert.True(wanted.SequenceEqual(got), $"{language}: placeholders differ for \"{key}\"");
                }
            }
        }
    }
}
