; The Windows installer: per-user, no admin rights, no internet.
; Compiled by tools/make-bundle.py --installer, which passes
;   /DAppVersion=<x.y.z> /DBundleDir=<the bundle folder> /DOutDir=<dist>

#ifndef AppVersion
  #define AppVersion "dev"
#endif

[Setup]
AppId={{6E1B0C0A-5B7D-4F0E-9B1E-0D5C7A2F4B11}
AppName=USDprep
AppVersion={#AppVersion}
AppPublisher=Petr Sajner
AppCopyright=Copyright (C) 2026 Petr Sajner
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Petr Sajner
VersionInfoProductName=USDprep
VersionInfoDescription=USDprep setup
LicenseFile={#BundleDir}\LICENSE.txt
DefaultDirName={localappdata}\Programs\USDprep
DefaultGroupName=USDprep
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=USDprep-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
InfoBeforeFile=requirements.txt
ChangesEnvironment=yes
UninstallDisplayIcon={app}\bin\USDprep.exe

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add the usdcut command line tool to my PATH"; Flags: unchecked

[Files]
Source: "{#BundleDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\USDprep"; Filename: "{app}\bin\USDprep.exe"; WorkingDir: "{userdocs}"
Name: "{group}\USDprep User Manual"; Filename: "{app}\USDprep_User_Manual.pdf"
Name: "{group}\Uninstall USDprep"; Filename: "{uninstallexe}"
Name: "{userdesktop}\USDprep"; Filename: "{app}\bin\USDprep.exe"; WorkingDir: "{userdocs}"; Tasks: desktopicon

[Registry]
; usdcut on the user's PATH (HKCU: no admin needed); removed again on uninstall
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Run]
Filename: "{app}\bin\USDprep.exe"; Description: "Start USDprep"; Flags: nowait postinstall skipifsilent
Filename: "{app}\USDprep_User_Manual.pdf"; Description: "Open the user manual"; Flags: shellexec nowait postinstall skipifsilent unchecked

[Code]
function NeedsAddPath(Dir: string): Boolean;
var
  Current: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Lowercase(Dir) + ';', ';' + Lowercase(Current) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Current, Dir: string;
  At: Integer;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then exit;
  Dir := ';' + ExpandConstant('{app}\bin');
  At := Pos(Lowercase(Dir), Lowercase(Current));
  if At > 0 then
  begin
    Delete(Current, At, Length(Dir));
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current);
  end;
end;
