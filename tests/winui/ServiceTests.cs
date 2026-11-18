using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using CastMirror.Services;
using Xunit;

namespace CastMirror.Tests
{
    public class UpdateServiceTests
    {
        [Theory]
        [InlineData("v1.2.3", 1, 2, 3)]
        [InlineData("1.2.3", 1, 2, 3)]
        [InlineData("V2.0.0-rc1", 2, 0, 0)]
        [InlineData("v3.4.5+build7", 3, 4, 5)]
        public void ParseTagAcceptsReleaseTags(string tag, int major, int minor, int patch)
        {
            Assert.Equal(new Version(major, minor, patch), UpdateService.ParseTag(tag));
        }

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("latest")]
        [InlineData("nightly-2026")]
        public void ParseTagRejectsNonVersions(string? tag)
        {
            Assert.Null(UpdateService.ParseTag(tag));
        }

        private sealed class FakeHandler : HttpMessageHandler
        {
            private readonly HttpStatusCode _status;
            private readonly string _body;
            public FakeHandler(HttpStatusCode status, string body) { _status = status; _body = body; }

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
                Task.FromResult(new HttpResponseMessage(_status) { Content = new StringContent(_body, Encoding.UTF8, "application/json") });
        }

        [Fact]
        public async Task NewerReleaseIsReported()
        {
            var handler = new FakeHandler(HttpStatusCode.OK,
                "{\"tag_name\":\"v99.0.0\",\"html_url\":\"https://example.test/r\"}");
            UpdateInfo? update = await UpdateService.CheckAsync(handler);
            Assert.NotNull(update);
            Assert.Equal(new Version(99, 0, 0), update!.Latest);
            Assert.Equal("https://example.test/r", update.PageUrl);
        }

        [Fact]
        public async Task SameOrOlderReleaseIsNotAnUpdate()
        {
            var handler = new FakeHandler(HttpStatusCode.OK, "{\"tag_name\":\"v0.0.1\"}");
            Assert.Null(await UpdateService.CheckAsync(handler));
        }

        [Fact]
        public async Task ServerErrorSurfacesInsteadOfClaimingUpToDate()
        {
            var handler = new FakeHandler(HttpStatusCode.Forbidden, "{}");
            await Assert.ThrowsAsync<HttpRequestException>(() => UpdateService.CheckAsync(handler));
        }
    }

    public class DiagnosticsServiceTests
    {
        [Fact]
        public void MaskAddressesHidesTheLastOctetOnly()
        {
            string masked = DiagnosticsService.MaskAddresses("connect 192.168.7.42:8009 via 10.0.0.5");
            Assert.Equal("connect 192.168.7.x:8009 via 10.0.0.x", masked);
        }

        [Fact]
        public void StripAnsiRemovesColourCodes()
        {
            Assert.Equal("[INFO ] hi", DiagnosticsService.StripAnsi("\u001b[32m[INFO ] hi\u001b[0m"));
        }

        [Fact]
        public void TailOfReturnsOnlyTheLastLinesEvenWhenAnotherProcessHoldsTheFile()
        {
            string path = Path.GetTempFileName();
            try
            {
                File.WriteAllLines(path, new[] { "a", "b", "c", "d", "e" });
                using var writer = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite);
                var tail = DiagnosticsService.TailOf(path, 2);
                Assert.Equal(new[] { "d", "e" }, tail);
            }
            finally
            {
                File.Delete(path);
            }
        }

        [Fact]
        public void TailOfMissingFileIsEmpty()
        {
            Assert.Empty(DiagnosticsService.TailOf(Path.Combine(Path.GetTempPath(), "no-such-castmirror.log"), 5));
        }

        [Fact]
        public void BuildMasksAddressesAndNamesTheSections()
        {
            string bundle = DiagnosticsService.Build("{\"ip\":\"192.168.1.20\"}");
            Assert.Contains("CastMirror diagnostics", bundle);
            Assert.Contains("192.168.1.x", bundle);
            Assert.DoesNotContain("192.168.1.20", bundle);
            Assert.Contains("Engine log", bundle);
            Assert.Contains("App log", bundle);
        }
    }
}

namespace CastMirror.Tests
{
    public class CrashHandlerTests
    {
        [Fact]
        public void PruneKeepsOnlyTheNewestDumps()
        {
            string dir = Path.Combine(Path.GetTempPath(), "castmirror-crash-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                for (int i = 0; i < 8; i++)
                {
                    string file = Path.Combine(dir, $"crash-2026010{i}-000000.dmp");
                    File.WriteAllText(file, "x");
                    File.SetLastWriteTimeUtc(file, new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddDays(i));
                }
                File.WriteAllText(Path.Combine(dir, "notes.txt"), "keep me");

                CrashHandler.PruneOldDumps(dir, 3);

                var left = Directory.GetFiles(dir, "crash-*.dmp");
                Assert.Equal(3, left.Length);
                Assert.Contains(Path.Combine(dir, "crash-20260107-000000.dmp"), left);
                Assert.True(File.Exists(Path.Combine(dir, "notes.txt")));
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }

        [Fact]
        public void PreviousCrashIsReportedOnceThenCleared()
        {
            string dir = Path.Combine(Path.GetTempPath(), "castmirror-marker-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                Assert.Null(CrashHandler.TakePreviousCrash(dir));
                File.WriteAllText(Path.Combine(dir, "last-crash.txt"), @"C:\x\crash-1.dmp");
                Assert.Equal(@"C:\x\crash-1.dmp", CrashHandler.TakePreviousCrash(dir));
                Assert.Null(CrashHandler.TakePreviousCrash(dir));
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }
    }

    public class CastCoreBridgeTests
    {
        [Fact]
        public void NativeStructLayoutMatchesCAbiContract()
        {
            RuntimeHelpers.RunClassConstructor(typeof(CastCoreBridge).TypeHandle);
            Assert.Equal(3u, CastCoreBridge.ExpectedAbiVersion);
        }

        [Fact]
        public void NativeTextRoundTripsUtf8WithNulTerminator()
        {
            const string sample = "Кухня TV — Café";
            byte[] encoded = NativeText.EncodeZ(sample);
            Assert.Equal((byte)0, encoded[^1]);
            Assert.Equal(sample, NativeText.Decode(encoded));

            byte[] noNul = Encoding.UTF8.GetBytes("Living Room");
            Assert.Equal("Living Room", NativeText.Decode(noNul));
            Assert.Equal(string.Empty, NativeText.Decode(new byte[] { 0, 65, 66 }));
        }
    }

    public class SettingsServiceTests
    {
        [Fact]
        public void PresetNamesParseCaseInsensitivelyAndEmitCanonicalCasing()
        {
            Assert.Equal("Auto", new CastMirrorSettings().QualityPreset);
            Assert.True(CastMirrorSettings.TryParsePresetName("cinema", out int idx));
            Assert.Equal((int)CastMirrorQualityPreset.Cinema, idx);
            Assert.Equal("Cinema", CastMirrorSettings.PresetName(idx));
            Assert.False(CastMirrorSettings.TryParsePresetName("unknown", out _));
        }

        [Fact]
        public void EffectiveBitrateCapUsesPresetDefaultUntilOverridden()
        {
            var settings = new CastMirrorSettings { QualityPreset = "Cinema" };
            Assert.Equal(16000u, settings.EffectiveBitrateCapKbps());

            settings.SetBitrateCapKbps(14000u);
            Assert.Equal(14000u, settings.BitrateKbpsCinema);
            Assert.Equal(14000u, settings.MaxBitrateKbps);
            Assert.Equal(14000u, settings.EffectiveBitrateCapKbps());
        }

        [Fact]
        public void JsonRoundTripPreservesEngineSchemaKeys()
        {
            const string json = """
                {
                  "quality_preset": "Game",
                  "max_bitrate_kbps": 9500,
                  "bitrate_kbps_game": 9500,
                  "capture_fps": 60,
                  "audio_enabled": false,
                  "audio_bitrate_bps": 128000,
                  "target_delay_ms": 150,
                  "latency_hud_enabled": true,
                  "show_cursor": false,
                  "audio_process_name": "spotify.exe",
                  "global_hotkeys": true,
                  "reconnect_window_s": 120,
                  "adaptive_resolution_enabled": false,
                  "subnet_scan_enabled": true,
                  "force_software_encode": true,
                  "enable_tray_on_startup": true,
                  "close_to_tray": false,
                  "notify_on_events": false,
                  "verify_device_cert": false,
                  "first_run_complete": true,
                  "ui_theme": "dark",
                  "window_width": 1280,
                  "window_height": 800,
                  "last_device_id": "tv-1",
                  "last_source_kind": "window",
                  "last_source_id": 42
                }
                """;
            var settings = JsonSerializer.Deserialize<CastMirrorSettings>(json)!;
            Assert.Equal("Game", settings.QualityPreset);
            Assert.Equal(9500u, settings.BitrateKbpsGame);
            Assert.Equal(60, settings.CaptureFps);
            Assert.False(settings.AudioEnabled);
            Assert.Equal(128000u, settings.AudioBitrateBps);
            Assert.Equal(150, settings.TargetDelayMs);
            Assert.True(settings.LatencyHudEnabled);
            Assert.False(settings.ShowCursor);
            Assert.Equal("spotify.exe", settings.AudioProcessName);
            Assert.True(settings.GlobalHotkeys);
            Assert.Equal(120, settings.ReconnectWindowSeconds);
            Assert.False(settings.AdaptiveResolutionEnabled);
            Assert.True(settings.SubnetScanEnabled);
            Assert.True(settings.ForceSoftwareEncode);
            Assert.True(settings.EnableTrayOnStartup);
            Assert.False(settings.CloseToTray);
            Assert.False(settings.NotifyOnEvents);
            Assert.False(settings.VerifyDeviceCert);
            Assert.True(settings.FirstRunComplete);
            Assert.Equal("dark", settings.UiTheme);
            Assert.Equal(1280, settings.WindowWidth);
            Assert.Equal(800, settings.WindowHeight);
            Assert.Equal("tv-1", settings.LastDeviceId);
            Assert.Equal("window", settings.LastSourceKind);
            Assert.Equal(42, settings.LastSourceId);
        }
    }
}