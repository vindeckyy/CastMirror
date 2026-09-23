using System;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CastMirror.Services
{
    /// <summary>
    /// Mirror of the engine's AppConfig as exposed by castmirror_get_config_json.
    /// Property names are the JSON keys, so the app and a hand-edited
    /// config.json stay interchangeable.
    /// </summary>
    public sealed class CastMirrorSettings
    {
        public const string AutoPresetKey = "auto";

        // Defaults mirror core `AppConfig` (core/include/castcore/config.h): if
        // Load() ever fails, the values this DTO would persist must not silently
        // change engine behaviour. 0 for the per-preset bitrates means "use the
        // profile default", exactly as the engine reads them.
        [JsonPropertyName("quality_preset")] public string QualityPreset { get; set; } = "auto";
        [JsonPropertyName("max_bitrate_kbps")] public uint MaxBitrateKbps { get; set; } = 8000;
        [JsonPropertyName("bitrate_kbps_auto")] public uint BitrateKbpsAuto { get; set; }
        [JsonPropertyName("bitrate_kbps_high")] public uint BitrateKbpsHigh { get; set; }
        [JsonPropertyName("bitrate_kbps_balanced")] public uint BitrateKbpsBalanced { get; set; }
        [JsonPropertyName("bitrate_kbps_smooth")] public uint BitrateKbpsSmooth { get; set; }
        [JsonPropertyName("bitrate_kbps_game")] public uint BitrateKbpsGame { get; set; }
        [JsonPropertyName("bitrate_kbps_cinema")] public uint BitrateKbpsCinema { get; set; }
        [JsonPropertyName("capture_fps")] public int CaptureFps { get; set; }
        [JsonPropertyName("audio_enabled")] public bool AudioEnabled { get; set; } = true;
        [JsonPropertyName("audio_bitrate_bps")] public uint AudioBitrateBps { get; set; } = 192000;
        [JsonPropertyName("silence_host_speakers")] public bool SilenceHostSpeakers { get; set; } = true;
        [JsonPropertyName("target_delay_ms")] public int TargetDelayMs { get; set; } = 200;
        [JsonPropertyName("latency_hud_enabled")] public bool LatencyHudEnabled { get; set; }
        [JsonPropertyName("adaptive_enabled")] public bool AdaptiveEnabled { get; set; } = true;
        [JsonPropertyName("adaptive_resolution_enabled")] public bool AdaptiveResolutionEnabled { get; set; } = true;
        [JsonPropertyName("subnet_scan_enabled")] public bool SubnetScanEnabled { get; set; }
        [JsonPropertyName("force_software_encode")] public bool ForceSoftwareEncode { get; set; }
        [JsonPropertyName("enable_tray_on_startup")] public bool EnableTrayOnStartup { get; set; }
        [JsonPropertyName("close_to_tray")] public bool CloseToTray { get; set; } = true;
        [JsonPropertyName("notify_on_events")] public bool NotifyOnEvents { get; set; } = true;
        [JsonPropertyName("verify_device_cert")] public bool VerifyDeviceCert { get; set; } = true;
        [JsonPropertyName("first_run_complete")] public bool FirstRunComplete { get; set; }
        [JsonPropertyName("ui_theme")] public string UiTheme { get; set; } = string.Empty;

        /// <summary>Bitrate cap currently in force for the selected preset.</summary>
        public uint BitrateCapKbps()
        {
            // The engine writes preset names capitalized ("High"); match them
            // case-insensitively so a hand-edited config also works.
            return (QualityPreset ?? string.Empty).Trim().ToLowerInvariant() switch
            {
                "high" => BitrateKbpsHigh,
                "balanced" => BitrateKbpsBalanced,
                "smooth" => BitrateKbpsSmooth,
                "game" => BitrateKbpsGame,
                "cinema" => BitrateKbpsCinema,
                _ => BitrateKbpsAuto
            };
        }

        /// <summary>
        /// Bitrate to show in the UI: the stored cap, or the preset's nominal
        /// default when the key is unset (0 means "engine default").
        /// </summary>
        public uint EffectiveBitrateCapKbps()
        {
            uint stored = BitrateCapKbps();
            if (stored != 0) return stored;
            return (QualityPreset ?? string.Empty).Trim().ToLowerInvariant() switch
            {
                "high" => 12000,
                "smooth" => 5000,
                "cinema" => 16000,
                _ => 8000
            };
        }

        /// <summary>Stores a bitrate cap for the selected preset (and its ceiling).</summary>
        public void SetBitrateCapKbps(uint kbps)
        {
            switch ((QualityPreset ?? string.Empty).Trim().ToLowerInvariant())
            {
                case "high": BitrateKbpsHigh = kbps; break;
                case "balanced": BitrateKbpsBalanced = kbps; break;
                case "smooth": BitrateKbpsSmooth = kbps; break;
                case "game": BitrateKbpsGame = kbps; break;
                case "cinema": BitrateKbpsCinema = kbps; break;
                default: BitrateKbpsAuto = kbps; break;
            }
            MaxBitrateKbps = kbps;
        }
    }

    /// <summary>
    /// Reads and writes the engine's persisted configuration. Every value the
    /// UI changes goes through here so the engine and the settings window never
    /// disagree about what is saved.
    /// </summary>
    public static class SettingsService
    {
        private static readonly JsonSerializerOptions Options = new()
        {
            WriteIndented = false,
            AllowTrailingCommas = true,
            ReadCommentHandling = JsonCommentHandling.Skip
        };

        /// <summary>Loads the engine configuration; returns defaults if unavailable.</summary>
        public static CastMirrorSettings Load()
        {
            try
            {
                int needed = CastCoreBridge.castmirror_get_config_json(null!, 0);
                if (needed <= 0) return new CastMirrorSettings();

                var buffer = new StringBuilder(needed);
                int written = CastCoreBridge.castmirror_get_config_json(buffer, buffer.Capacity);
                if (written <= 0) return new CastMirrorSettings();

                return JsonSerializer.Deserialize<CastMirrorSettings>(buffer.ToString(), Options)
                       ?? new CastMirrorSettings();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
                return new CastMirrorSettings();
            }
        }

        /// <summary>Persists the configuration. Returns false when the engine rejected it.</summary>
        public static bool Save(CastMirrorSettings settings)
        {
            try
            {
                return CastCoreBridge.castmirror_set_config_json(JsonSerializer.Serialize(settings, Options));
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
                return false;
            }
        }

        /// <summary>Runs the engine's self-test and returns its JSON result.</summary>
        public static string RunSelfTest()
        {
            try
            {
                int needed = CastCoreBridge.castmirror_self_test(null!, 0);
                if (needed <= 0) return string.Empty;
                var buffer = new StringBuilder(needed);
                int written = CastCoreBridge.castmirror_self_test(buffer, buffer.Capacity);
                return written > 0 ? buffer.ToString() : string.Empty;
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
                return string.Empty;
            }
        }
    }
}
