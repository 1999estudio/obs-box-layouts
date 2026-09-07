#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#ifndef SourceDir
  #error SourceDir must point to the installed obs-box-layouts directory
#endif

#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{16B13EEB-55C1-471C-B328-6FE61AC83020}
AppName=OBS Box Layouts
AppVersion={#MyAppVersion}
AppPublisher=OBS Box Layouts contributors
AppPublisherURL=https://github.com/1999estudio/obs-box-layouts
DefaultDirName={commonappdata}\obs-studio\plugins\obs-box-layouts
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
UsePreviousAppDir=no
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename=obs-box-layouts-{#MyAppVersion}-windows-x64-installer
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\64bit\obs-box-layouts.dll
CloseApplications=no
RestartApplications=no

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Remove the incorrect per-user location used by version 0.7.0.
Type: filesandordirs; Name: "{userappdata}\obs-studio\plugins\obs-box-layouts"
