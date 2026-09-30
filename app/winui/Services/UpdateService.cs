using System;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;

namespace CastMirror.Services
{
    /// <summary>The newest published release, when it is newer than the running build.</summary>
    public sealed record UpdateInfo(Version Latest, string PageUrl);

    /// <summary>
    /// Asks GitHub whether a newer release exists. This runs only when the user
    /// clicks "Check for updates"; CastMirror never contacts a server on its own.
    /// </summary>
    public static class UpdateService
    {
        private const string LatestReleaseUrl =
            "https://api.github.com/repos/vindeckyy/CastMirror/releases/latest";

        public static Version Current =>
            typeof(UpdateService).Assembly.GetName().Version ?? new Version(1, 0, 0);

        /// <summary>Parses "v1.2.3" or "1.2.3"; null for anything else.</summary>
        public static Version? ParseTag(string? tag)
        {
            if (string.IsNullOrWhiteSpace(tag)) return null;
            string text = tag.Trim().TrimStart('v', 'V');
            int dash = text.IndexOfAny(new[] { '-', '+' });
            if (dash >= 0) text = text[..dash];
            return Version.TryParse(text, out Version? version) ? version : null;
        }

        /// <summary>
        /// Returns the update, or null when the running build is current.
        /// Throws when the check itself failed (offline, rate limited), so the
        /// caller can say so instead of claiming "up to date".
        /// </summary>
        public static async Task<UpdateInfo?> CheckAsync(HttpMessageHandler? handler = null)
        {
            using var client = handler == null ? new HttpClient() : new HttpClient(handler);
            client.Timeout = TimeSpan.FromSeconds(10);
            client.DefaultRequestHeaders.UserAgent.ParseAdd($"CastMirror/{Current.ToString(3)}");
            client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");

            using HttpResponseMessage response = await client.GetAsync(LatestReleaseUrl);
            response.EnsureSuccessStatusCode();
            using JsonDocument document = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            JsonElement root = document.RootElement;

            Version? latest = ParseTag(root.GetProperty("tag_name").GetString());
            if (latest == null) throw new InvalidOperationException("The release has no version number.");
            if (latest <= new Version(Current.Major, Current.Minor, Math.Max(Current.Build, 0))) return null;

            string url = root.TryGetProperty("html_url", out JsonElement page)
                ? page.GetString() ?? "https://github.com/vindeckyy/CastMirror/releases"
                : "https://github.com/vindeckyy/CastMirror/releases";
            return new UpdateInfo(latest, url);
        }
    }
}
