; WinConsoleServer / WinConsoleTerminal installer
; Inno Setup 6

#define MyAppName "WinConsoleServer"
#define MyAppVersion "0.2.0"
#define MyAppPublisher "WinConsoleServer Project"
#define MyAppURL "https://example.invalid/WinConsoleServer"
#define MyAppExeName "WinConsoleTerminal.exe"

[Setup]
AppId={{8B2E3F5A-4C1D-4F4E-9A3B-7E5C2D1F0A11}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=
OutputDir=..\dist
OutputBaseFilename=WinConsoleServerSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
MinVersion=10.0.17763
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "firewall"; Description: "Add Windows Firewall rule for TCP 7000 (server)"; GroupDescription: "Network:"; Flags: checkedonce

[Files]
Source: "..\bin\Release\WinConsoleServer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\Release\WinConsoleTerminal.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\conpty\conpty.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\conpty\OpenConsole.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\doc\design.md"; DestDir: "{app}\doc"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\WinConsoleTerminal"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\WinConsoleServer (cmd, port 7000)"; Filename: "{app}\WinConsoleServer.exe"; Parameters: "--listen 0.0.0.0:7000 --shell cmd"; WorkingDir: "{app}"
Name: "{group}\WinConsoleServer (bash, port 7000)"; Filename: "{app}\WinConsoleServer.exe"; Parameters: "--listen 0.0.0.0:7000 --shell bash"; WorkingDir: "{app}"
Name: "{group}\Design document"; Filename: "{app}\doc\design.md"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\WinConsoleTerminal"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""WinConsoleServer TCP 7000"" dir=in action=allow protocol=TCP localport=7000"; Flags: runhidden; Tasks: firewall
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinConsoleServer TCP 7000"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
