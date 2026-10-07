[Setup]
AppId={{D2AD37A2-4579-4A2D-8E23-A7D9A67359B2}
AppName=PhoneCam
AppVersion=0.2.0
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
Source: "..\..\build\windows\Release\PhoneCamSource.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace

[Registry]
Root: HKLM; Subkey: "Software\Classes\CLSID\{{B6E2A98D-6F02-4C98-86F8-5AE30A2D17C2}"; ValueType: string; ValueName: ""; ValueData: "PhoneCam virtual camera media source"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\CLSID\{{B6E2A98D-6F02-4C98-86F8-5AE30A2D17C2}\InProcServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\PhoneCamSource.dll"
Root: HKLM; Subkey: "Software\Classes\CLSID\{{B6E2A98D-6F02-4C98-86F8-5AE30A2D17C2}\InProcServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Both"

[Icons]
Name: "{group}\PhoneCam"; Filename: "{app}\PhoneCam.exe"
Name: "{autodesktop}\PhoneCam"; Filename: "{app}\PhoneCam.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Run]
Filename: "{app}\PhoneCam.exe"; Description: "Launch PhoneCam"; Flags: nowait postinstall skipifsilent
