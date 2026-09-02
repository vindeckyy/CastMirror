#ifndef CASTMIRROR_GUI_HELP_COPY_H_
#define CASTMIRROR_GUI_HELP_COPY_H_

namespace castcore::gui::copy {

// Application & Header
inline constexpr const char* kAppTitle = "CastMirror";
inline constexpr const char* kAppSubtitleDefault = "Native display mirroring for Cast devices";
inline constexpr const char* kReadySubtitle = "Choose a TV and screen, then cast your display.";
inline constexpr const char* kAboutComments =
    "Native display mirroring for Linux. Not affiliated with Google. Chromecast, Google Cast, and Google TV are Google trademarks.";
inline constexpr const char* kAboutLicense =
    "Source Apache-2.0. The official .deb is GPL because it links distro libx264. See NOTICE.";

// Cast Tab - Sections
inline constexpr const char* kSectionTelevision = "Where to cast";
inline constexpr const char* kSectionTelevisionHelp = "Choose the Google Cast, Android TV, or Nest display on your LAN.";
inline constexpr const char* kSectionDisplay = "What to share";
inline constexpr const char* kSectionDisplayHelp = "CastMirror sends the selected screen or window, cropped to its XRandR / portal rectangle — not a browser tab.";
inline constexpr const char* kWaylandPortalNote =
    "On Wayland, clicking Cast will open the desktop portal so you can pick a screen or window. CastMirror then matches that size before talking to the TV.";
inline constexpr const char* kSectionQuality = "Picture quality";
inline constexpr const char* kSectionQualityHelp = "Pick how sharp and fluid the picture should be. You can customize the bitrate cap in Settings.";
inline constexpr const char* kQualityPopover =
    "Chromecast mirroring is designed for desktop productivity, presentations, and video playback (~200–400 ms playout delay). It is not a low-latency game streamer like Sunshine/Moonlight.";
inline constexpr const char* kSectionSound = "Sound";
inline constexpr const char* kSectionSoundHelp = "System audio (the default PulseAudio / PipeWire monitor sink), not a microphone.";
inline constexpr const char* kComputerSoundTitle = "Computer sound";
inline constexpr const char* kComputerSoundHelp = "Play system audio on the TV while casting";
inline constexpr const char* kSoundPopover =
    "Sends desktop audio using low-latency Opus. Turn on 'Mute sound while streaming' if you only want sound from the television.";

// Device List & Empty State
inline constexpr const char* kEmptyDevicesDetail =
    "CastMirror uses mDNS to find video-capable Cast devices. If your router blocks mDNS, use Add by IP or enable LAN scanning in Settings. Audio-only speakers are hidden.";
inline constexpr const char* kEmptyDevicesTitle = "No TVs found yet";
inline constexpr const char* kEmptyDevicesBody =
    "1. Make sure your TV or Chromecast is powered on.\n"
    "2. Ensure this PC and the TV are on the exact same Wi-Fi or wired network.\n"
    "3. Wait a few seconds or click Rescan.\n"
    "4. If mDNS is blocked by your router, use 'Add by IP' or enable LAN scan in Settings.\n\n"
    "Note: Audio-only Cast speakers (like Google Home Mini) are hidden because they cannot show video.";
inline constexpr const char* kRescanTooltip = "Rescan LAN for Google Cast devices via mDNS";
inline constexpr const char* kAddIpTooltip = "Add a custom Cast device by IPv4 address and port";
inline constexpr const char* kRemoveDeviceTooltip = "Remove this saved manual device from the list";

// Presets
inline constexpr const char* kPresetAutoTitle = "Auto";
inline constexpr const char* kPresetAutoDesc = "Lets CastMirror pick the starting quality from this TV's model, then adapt if Wi-Fi struggles.";
inline constexpr const char* kPresetHighTitle = "High";
inline constexpr const char* kPresetHighDesc = "Sharpest picture this TV allows (up to 1080p60, or 4K30 on Ultra / Google TV). Uses more Wi-Fi.";
inline constexpr const char* kPresetBalancedTitle = "Balanced";
inline constexpr const char* kPresetBalancedDesc = "1080p-class picture at a moderate bitrate. Best default if Auto feels wasteful.";
inline constexpr const char* kPresetSmoothTitle = "Smooth";
inline constexpr const char* kPresetSmoothDesc = "720p and a lower bitrate so motion stays fluid on busy Wi-Fi.";

// Live Session Tab
inline constexpr const char* kLiveEmptyTitle = "Nothing is being sent";
inline constexpr const char* kLiveEmptyBody =
    "Start from the Cast tab. Capture does not run until a session is live, so your screen is never shared in the background.";
inline constexpr const char* kHealthHealthy = "Stream looks healthy.";
inline constexpr const char* kLadderCaption =
    "Adaptive steps down this ladder when Wi-Fi is lossy, then ramps your bitrate back to the target once the link recovers. It will not go above the quality you picked or this TV's limit. A ladder change does not reconnect — the TV just gets a new keyframe.";
inline constexpr const char* kLadderDisabledCaption =
    "Adaptive quality is always on. The encoder holds your bitrate and ramps back to it after any congestion drop.";

// Stat Tile Help
inline constexpr const char* kStatFpsHelp = "How many video frames the PC is capturing and transmitting each second.";
inline constexpr const char* kStatBitrateHelp = "Current encoded video bitrate. Adaptive bitrate may operate below your configured ceiling.";
inline constexpr const char* kStatRttHelp = "Round-trip time for TV RTCP feedback packets. High latency indicates Wi-Fi congestion.";
inline constexpr const char* kStatLossHelp = "Percentage of RTP video packets reported missing by the TV receiver.";
inline constexpr const char* kStatDelayHelp = "Target buffer duration the TV holds before displaying frames to absorb network jitter.";
inline constexpr const char* kStatSizeHelp = "Actual encoded resolution and framerate after aspect ratio letterboxing and adaptive scaling.";
inline constexpr const char* kStatEncoderHelp =
    "Active video encoder. 'h264_vaapi' uses GPU hardware; 'libx264' uses CPU software. If the TV screen is black with VAAPI, enable 'Force software encode' in Settings.";
inline constexpr const char* kStatRepairsHelp = "NACK: Packets resent upon TV request. PLI: Instant keyframe requests upon frame loss.";
inline constexpr const char* kStatSentHelp = "Total video frames and RTP media packets transmitted since connection began.";

// Settings Tab
inline constexpr const char* kBitrateTitle = "Video bitrate cap";
inline constexpr const char* kBitrateHelp =
    "Bitrate CastMirror holds while streaming. Drops only on network congestion and ramps back up quickly when the link recovers.";
inline constexpr const char* kBitratePopover =
    "Applies immediately while Live without reconnecting. CastMirror holds this bitrate on a clean link, drops below it only when the TV reports packet loss, and aggressively ramps back up to your target once the connection recovers.";

inline constexpr const char* kFpsTitle = "Capture frame rate";
inline constexpr const char* kFpsHelp = "0 means follow the monitor's refresh rate, clamped to what the TV supports.";
inline constexpr const char* kFpsPopover =
    "Most Chromecast devices only accept 30 or 60 fps. Framerates above 60 are rejected by the TV decoder.";

inline constexpr const char* kAudioQualityTitle = "Audio quality";
inline constexpr const char* kAudioQualityHelp = "Opus bitrate. 192 kbps is recommended; 64 kbps is best for weak Wi-Fi.";

inline constexpr const char* kSilenceTitle = "Mute sound while streaming";
inline constexpr const char* kSilenceHelp = "Stops the room from playing the same audio twice. Speaker volume is restored when stopped.";

inline constexpr const char* kDelayTitle = "Target delay";
inline constexpr const char* kDelayHelp = "How long the TV waits before drawing each frame (buffer depth).";
inline constexpr const char* kDelayPopover =
    "200 ms gives responsive cursor movement; 400 ms is Chrome's default buffer to withstand Wi-Fi packet retransmits.";

inline constexpr const char* kSubnetScanTitle = "Scan LAN for silent TVs";
inline constexpr const char* kSubnetScanHelp = "TCP-probes port 8009 on your local /24 subnet if mDNS discovery is blocked.";
inline constexpr const char* kSubnetScanConfirm =
    "This sends a short TCP connection probe to every IP address on your local subnet on port 8009.\n\n"
    "Leave this off on corporate, school, or guest Wi-Fi networks.\n\n"
    "Do you want to enable subnet scanning?";

inline constexpr const char* kForceX11Title = "Force X11 capture";
inline constexpr const char* kForceX11Help = "Skip the Wayland portal and use XRandR + MIT-SHM capture directly.";
inline constexpr const char* kForceX11Popover =
    "Equivalent to CASTMIRROR_FORCE_X11=1. On a pure Wayland compositor without XWayland, this falls back to synthetic video.";

inline constexpr const char* kForceSoftwareTitle = "Force software encode";
inline constexpr const char* kForceSoftwareHelp = "Never use VAAPI GPU acceleration; always encode with libx264 CPU.";
inline constexpr const char* kForceSoftwarePopover =
    "Use this if your TV screen stays black while logs report 'h264_vaapi'. Useful on buggy GPU drivers or virtual machines.";

inline constexpr const char* kTrayTitle = "Show a system tray icon";
inline constexpr const char* kTrayHelp = "Lets Cast keep running even if you close the application window.";

inline constexpr const char* kCloseToTrayTitle = "Closing window hides to tray";
inline constexpr const char* kCloseToTrayHelp = "When enabled, clicking the window close button minimizes to the system tray instead of quitting.";

inline constexpr const char* kNotifyTitle = "Desktop notifications";
inline constexpr const char* kNotifyHelp = "Shows desktop notifications when casting starts, disconnects, or reconnects.";

// First Run Assistant
inline constexpr const char* kFirstRunWelcomeTitle = "Welcome to CastMirror";
inline constexpr const char* kFirstRunWelcomeBody =
    "CastMirror sends your Linux desktop and audio directly to Google Cast TVs and Chromecast dongles.\n\n"
    "• Native real-time streaming (no web browser needed)\n"
    "• Hardware-accelerated H.264 encoding with adaptive bitrate\n"
    "• Typical latency is around 200–400 milliseconds (standard Chromecast buffer depth)";

inline constexpr const char* kFirstRunNetworkTitle = "Network connection";
inline constexpr const char* kFirstRunNetworkBody =
    "Make sure your PC and Chromecast TV are connected to the same Wi-Fi or wired network.\n\n"
    "• CastMirror uses mDNS to automatically detect nearby TVs.\n"
    "• If your TV is on a different VLAN or subnet, you can add it manually using 'Add by IP' on the Cast tab.";

inline constexpr const char* kFirstRunCaptureTitle = "Select your screen";
inline constexpr const char* kFirstRunCaptureBody =
    "On the Cast tab, pick your target TV and screen, then click 'Cast display'.\n\n"
    "• On Wayland desktops, a system dialog will appear asking you to choose the monitor or window to share.\n"
    "• Screen capture only runs while actively casting and stops immediately when you click Stop.";

}  // namespace castcore::gui::copy

#endif  // CASTMIRROR_GUI_HELP_COPY_H_
