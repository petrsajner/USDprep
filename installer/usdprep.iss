; The Windows installer: per-user, no admin rights, no internet.
; Compiled by tools/make-bundle.py --installer, which passes
;   /DAppVersion=<x.y.z> /DBundleDir=<the bundle folder> /DOutDir=<dist>

#ifndef AppVersion
  #define AppVersion "dev"
#endif

[Setup]
AppId={{6E1B0C0A-5B7D-4F0E-9B1E-0D5C7A2F4B11}
AppName=USD Prep for Nuke
AppVersion={#AppVersion}
AppPublisher=usdprep
DefaultDirName={localappdata}\Programs\usdprep
DefaultGroupName=USD Prep for Nuke
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=usdprep-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
InfoBeforeFile=requirements.txt
ChangesEnvironment=yes
UninstallDisplayIcon={app}\bin\usdtweak.exe

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add the usdcut command line tool to my PATH"; Flags: unchecked

[Files]
Source: "{#BundleDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\USD Prep for Nuke"; Filename: "{app}\bin\usdtweak.exe"; WorkingDir: "{userdocs}"
Name: "{group}\Uninstall USD Prep for Nuke"; Filename: "{uninstallexe}"
Name: "{userdesktop}\USD Prep for Nuke"; Filename: "{app}\bin\usdtweak.exe"; WorkingDir: "{userdocs}"; Tasks: desktopicon

[Registry]
; usdcut on the user's PATH (HKCU: no admin needed); removed again on uninstall
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Run]
Filename: "{app}\bin\usdtweak.exe"; Description: "Start USD Prep for Nuke"; Flags: nowait postinstall skipifsilent

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
