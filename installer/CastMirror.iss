; CastMirror Windows installer (Inno Setup 6).
;
; Build:  iscc installer\CastMirror.iss /DAppVersion=1.0.0
; Input:  publish\ (produced by package.ps1) - the app, castcore.dll and its runtime DLLs.
;
; Installs per machine, so the firewall rules and the Start menu entry cover every
; user on the PC. Nothing here phones home.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define AppName "CastMirror"
#define AppExe "CastMirror.exe"
#define FirewallRuleIn "CastMirror (inbound)"
#define FirewallRuleOut "CastMirror (outbound)"

[Setup]
AppId={{6B2F5C7E-3D1A-4E58-9C0B-CA57A1D2E9F4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=CastMirror Maintainers
AppPublisherURL=https://github.com/vindeckyy/CastMirror
AppSupportURL=https://github.com/vindeckyy/CastMirror/issues
AppUpdatesURL=https://github.com/vindeckyy/CastMirror/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
OutputDir=..\dist
OutputBaseFilename=CastMirror-Setup-{#AppVersion}-x64
SetupIconFile=..\app\winui\CastMirror.ico
LicenseFile=..\publish\LICENSE
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
PrivilegesRequired=admin
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "autostart"; Description: "Start {#AppName} hidden in the tray when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "..\publish\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Per-user Run value, the same one the Settings switch manages.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
  ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --background"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
; Discovery (mDNS) and the media stream need inbound UDP. Without these the first cast
; triggers a Windows Firewall prompt that many users dismiss, and then nothing is found.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#FirewallRuleIn}"" dir=in action=allow program=""{app}\{#AppExe}"" enable=yes profile=private,domain"; \
  Flags: runhidden; StatusMsg: "Allowing CastMirror through Windows Firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#FirewallRuleOut}"" dir=out action=allow program=""{app}\{#AppExe}"" enable=yes profile=private,domain"; \
  Flags: runhidden
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FirewallRuleIn}"""; Flags: runhidden; RunOnceId: "DelFwIn"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#FirewallRuleOut}"""; Flags: runhidden; RunOnceId: "DelFwOut"

[Code]
// Settings, the saved window size and logs live in %APPDATA%\CastMirror. Keep them by
// default so a reinstall picks up where the user left off; offer to remove them.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{userappdata}\CastMirror');
    if DirExists(DataDir) and (not UninstallSilent) then
    begin
      if MsgBox('Also delete your CastMirror settings and logs?' + #13#10 + DataDir,
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
