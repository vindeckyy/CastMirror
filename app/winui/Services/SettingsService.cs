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

        // The single name↔ordinal mapping for the engine's QualityPreset
        // (castcore's preset list order); index 0 is Auto. These are the
        // canonical spellings castcore's QualityPresetToString emits.
        private static readonly string[] PresetNames =
            { "Auto", "High", "Balanced", "Smooth", "Game", "Cinema" };

        /// <summary>
        /// Maps an engine preset name to its ordinal. Matching is
        /// case-insensitive because hand-edited configs tend to use lower
        /// case even though castcore's QualityPresetFromString only accepts
        /// the canonical capitalized spellings.
        /// </summary>
        public static bool TryParsePresetName(string? name, out int index)
        {
            index = -1;
            if (string.IsNullOrWhiteSpace(name)) return false;
            for (int i = 0; i < PresetNames.Length; ++i)
            {
                if (string.Equals(name.Trim(), PresetNames[i],
                                  StringComparison.OrdinalIgnoreCase))
                {
                    index = i;
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// Canonical preset name for an ordinal ("High", ...), or "Auto" when
        /// the index is out of range. Use this — not a hand-cased literal —
        /// when writing quality_preset: the engine's parser is case-sensitive
        /// and reads anything else (including "high") as auto.
        /// </summary>
        public static string PresetName(int index) =>
            index >= 0 && index < PresetNames.Length
                ? PresetNames[index]
                : PresetNames[0];

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
        [JsonPropertyName("show_cursor")] public bool ShowCursor { get; set; } = true;

        // Window size in DIPs. The engine's own default (920x700) doubles as
        // "never saved", so the client falls back to its 1100x720 layout then.
        [JsonPropertyName("window_width")] public int WindowWidth { get; set; } = 920;
        [JsonPropertyName("window_height")] public int WindowHeight { get; set; } = 700;

        // Recorded by the engine when a cast starts; the client only reads them,
        // to preselect the same TV and source on the next launch.
        [JsonPropertyName("last_device_id")] public string LastDeviceId { get; set; } = string.Empty;
        [JsonPropertyName("last_source_kind")] public string LastSourceKind { get; set; } = "monitor";
        [JsonPropertyName("last_source_id")] public int LastSourceId { get; set; }

        /// <summary>Bitrate cap currently in force for the selected preset.</summary>
        public uint BitrateCapKbps()
        {
            if (!TryParsePresetName(QualityPreset, out int index)) return BitrateKbpsAuto;
            return (CastMirrorQualityPreset)index switch
            {
                CastMirrorQualityPreset.High => BitrateKbpsHigh,
                CastMirrorQualityPreset.Balanced => BitrateKbpsBalanced,
                CastMirrorQualityPreset.Smooth => BitrateKbpsSmooth,
                CastMirrorQualityPreset.Game => BitrateKbpsGame,
                CastMirrorQualityPreset.Cinema => BitrateKbpsCinema,
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
            if (!TryParsePresetName(QualityPreset, out int index)) return 8000;
            return (CastMirrorQualityPreset)index switch
            {
                CastMirrorQualityPreset.High => 12000,
                CastMirrorQualityPreset.Smooth => 5000,
                CastMirrorQualityPreset.Cinema => 16000,
                _ => 8000
            };
        }

        /// <summary>Stores a bitrate cap for the selected preset (and its ceiling).</summary>
        public void SetBitrateCapKbps(uint kbps)
        {
            CastMirrorQualityPreset preset =
                TryParsePresetName(QualityPreset, out int index)
                    ? (CastMirrorQualityPreset)index
                    : CastMirrorQualityPreset.Auto;
            switch (preset)
            {
                case CastMirrorQualityPreset.High: BitrateKbpsHigh = kbps; break;
                case CastMirrorQualityPreset.Balanced: BitrateKbpsBalanced = kbps; break;
                case CastMirrorQualityPreset.Smooth: BitrateKbpsSmooth = kbps; break;
                case CastMirrorQualityPreset.Game: BitrateKbpsGame = kbps; break;
                case CastMirrorQualityPreset.Cinema: BitrateKbpsCinema = kbps; break;
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

        /// <summary>
        /// Loads the engine configuration.
        /// </summary>
        /// <remarks>
        /// This throws on failure instead of substituting defaults. Returning a
        /// default-valued DTO used to be silently fatal: every caller persists
        /// whatever it holds, so one truncated or unparsable read rewrote the
        /// user's entire configuration with the defaults. Callers must surface
        /// the failure and leave the existing settings untouched.
        /// </remarks>
        public static CastMirrorSettings Load()
        {
            string json = ReadConfigJson();
            CastMirrorSettings? settings =
                JsonSerializer.Deserialize<CastMirrorSettings>(json, Options);
            if (settings == null)
            {
                throw new InvalidOperationException(
                    "The engine returned an empty configuration document.");
            }
            return settings;
        }

        /// <summary>
        /// Reads a JSON document from the engine with the documented two-step
        /// contract (query the required size, then fill), growing and retrying
        /// while the engine reports that the document did not fit.
        /// </summary>
        /// <remarks>
        /// Shared by the configuration and the self-test: they expose the same
        /// buffer contract, and a self-test read that skipped the grow-and-retry
        /// returned truncated JSON that the diagnostics pane then displayed as
        /// raw partial text.
        /// </remarks>
        private static string ReadNativeJson(string what, Func<int> sizeQuery,
                                             Func<byte[], int, int> fill)
        {
            int needed = sizeQuery();
            if (needed <= 0)
            {
                throw new InvalidOperationException($"The engine did not report a {what} size.");
            }

            for (int attempt = 0; attempt < 4; ++attempt)
            {
                var buffer = new byte[needed];
                int written = fill(buffer, buffer.Length);
                if (written <= 0)
                {
                    throw new InvalidOperationException($"The engine returned no {what}.");
                }

                // A write that exactly filled the buffer may have been truncated
                // by the engine; grow the buffer and ask again rather than
                // parsing a partial document.
                if (written < buffer.Length - 1)
                {
                    return NativeText.Decode(buffer);
                }
                needed = buffer.Length * 2;
            }

            throw new InvalidOperationException($"The {what} did not fit in the buffer.");
        }

        private static string ReadConfigJson() =>
            ReadNativeJson(
                "configuration",
                () => CastCoreBridge.castmirror_get_config_json(null!, 0),
                (buffer, length) => CastCoreBridge.castmirror_get_config_json(buffer, length));

        /// <summary>Persists the configuration. Returns false when the engine rejected it.</summary>
        public static bool Save(CastMirrorSettings settings)
        {
            try
            {
                // UTF-8 on both sides: the engine parses the bytes as UTF-8, so
                // encoding them through the ANSI code page would corrupt any
                // non-ASCII string value.
                return CastCoreBridge.castmirror_set_config_json(
                    NativeText.EncodeZ(JsonSerializer.Serialize(settings, Options)));
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
                return ReadNativeJson(
                    "self-test result",
                    () => CastCoreBridge.castmirror_self_test(null!, 0),
                    (buffer, length) => CastCoreBridge.castmirror_self_test(buffer, length));
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
                return string.Empty;
            }
        }
    }
}
