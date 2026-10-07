[Setup]
AppId={{D2AD37A2-4579-4A2D-8E23-A7D9A67359B2}
AppName=PhoneCam
AppVersion=0.1.0
AppPublisher=soshroom
DefaultDirName={autopf}\PhoneCam
DefaultGroupName=PhoneCam
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=output
OutputBaseFilename=PhoneCamSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "..\..\build\windows\Release\PhoneCam.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\PhoneCam"; Filename: "{app}\PhoneCam.exe"
Name: "{autodesktop}\PhoneCam"; Filename: "{app}\PhoneCam.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Run]
Filename: "{app}\PhoneCam.exe"; Description: "Launch PhoneCam"; Flags: nowait postinstall skipifsilent
