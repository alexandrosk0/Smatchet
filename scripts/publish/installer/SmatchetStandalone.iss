#define MyAppName "Smatchet"
#define MyAppExeName "Smatchet.exe"

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#ifndef MySourceDir
  #error "MySourceDir must point at the staged standalone payload."
#endif

#ifndef MyOutputDir
  #error "MyOutputDir must point at the release assets directory."
#endif

#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "Smatchet-setup"
#endif

#ifndef MyInnoSignTool
  #define MyInnoSignTool ""
#endif

[Setup]
AppId={{6A63A3FA-86B2-4574-B0F6-7C8781B10C0F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Smatchet
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\..\..\SourceStandalone\smatchet.ico
#if MyInnoSignTool != ""
SignTool={#MyInnoSignTool}
SignedUninstaller=yes
#endif

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
