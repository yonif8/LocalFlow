#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\..\build\windows-stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\build\windows-package"
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "LocalFlow-windows-x64-setup"
#endif

#define AppName "LocalFlow"
#define AppPublisher "LocalFlow"
#define AppExeRelativePath "bin\LocalFlow.exe"

[Setup]
AppId={{6986D047-5C86-429C-90B1-E68833362648}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL=https://github.com/yonif8/LocalFlow
AppSupportURL=https://github.com/yonif8/LocalFlow/issues
AppUpdatesURL=https://github.com/yonif8/LocalFlow/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
Uninstallable=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeRelativePath}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} per-user installer
VersionInfoProductName={#AppName}
VersionInfoProductTextVersion={#AppVersion}

[Tasks]
Name: "startup"; Description: "Start LocalFlow when I sign in"; GroupDescription: "Additional options:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\LocalFlow"; Filename: "{app}\{#AppExeRelativePath}"; WorkingDir: "{app}\bin"
Name: "{userstartup}\LocalFlow"; Filename: "{app}\{#AppExeRelativePath}"; WorkingDir: "{app}\bin"; Tasks: startup

[Run]
Filename: "{app}\{#AppExeRelativePath}"; Description: "Start LocalFlow"; WorkingDir: "{app}\bin"; Flags: nowait postinstall skipifsilent
