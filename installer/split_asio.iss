; Inno Setup 6 Script for SplitASIO (Windows x64)

#define MyAppName "SplitASIO"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.1"
#endif
#define MyAppPublisher "SplitASIO"
#define MyAppExeName "split_asio_config.exe"

[Setup]
AppId={{7A4E9C21-3B8D-4F62-9E15-8C04A2B7D631}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=SplitASIO-Setup-{#MyAppVersion}-x64
SetupIconFile=..\assets\split_asio.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; ASIO COM driver DLL: regserver automatically calls DllRegisterServer on install and DllUnregisterServer on uninstall
Source: "..\build\Release\split_asio.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete
; Standalone settings utility
Source: "..\build\Release\split_asio_config.exe"; DestDir: "{app}"; Flags: ignoreversion
; Documentation
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\SplitASIO Settings"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall SplitASIO"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Configure SplitASIO input/output devices now"; Flags: nowait postinstall skipifsilent
