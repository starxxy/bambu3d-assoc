; Bambu3DAssoc installer script (Inno Setup 6)
; Build the binaries first:
;   cmake -S .. -B ..\build -G "Visual Studio 17 2022" -A x64
;   cmake --build ..\build --config Release
; Then compile this script with ISCC.exe; outputs Setup to installer\Output.
;
; v1.2.4 changes (uninstaller-only; the binaries are unchanged):
;   * The uninstaller now removes every trace, not just the association:
;       - the emptied <ext>\ShellEx and SystemFileAssociations\<ext>\ShellEx
;         parents (30 keys) are deleted once they are empty
;       - the per-user "open with" records Explorer had stored for our ProgID
;         and for bs3d_assoc.exe are dropped (they dangle after uninstall)
;   * The Windows thumbnail cache is cleared on uninstall. Explorer answers a
;     thumbnail request straight out of thumbcache_*.db and never re-checks
;     whether a handler is still registered, so .3mf/.stl previews used to
;     survive the uninstall and made it look like nothing had been removed.
;     The cache is only touched when our handler was actually registered.
;   * SHChangeNotify(SHCNE_ASSOCCHANGED) is raised so Explorer drops its
;     association/icon state immediately instead of at the next logon.
;
; v1.2.3 changes:
;   * Flicker fix: WS_CLIPCHILDREN on all three top-level windows, whole-window
;     double buffering in every WM_PAINT, no more InvalidateRect(..., TRUE).
;     The wizard no longer paints inside WM_ERASEBKGND.
;   * The advanced-settings preview box (.prev) is drawn by the window itself
;     again -- the SS_BITMAP STATIC child swallowed the caption strip.
;
; v1.2.2 changes:
;   * The install scope is now chosen inside the installer (user / machine)
;     instead of a toggle buried in the app's advanced settings.
;     PrivilegesRequired=lowest + PrivilegesRequiredOverridesAllowed=dialog
;     makes Setup ask up front; {autopf} / {group} / {autodesktop} then resolve
;     to the Program Files or the per-user location automatically, and
;     IsAdminInstallMode() tells [Registry] which root to stamp.
;   * The OCCT CAD kernel (bs3d_occ.dll + TK* runtime DLLs) now ALWAYS ships
;     inside Setup.exe -> installer\occt\ must be populated before compiling.
;     The runtime-download route (occpack.url) is gone.
;   * The single Setup.exe is the ONLY user-facing deliverable.

#define MyAppName      "Bambu 3D 文件关联管理器"
#define MyAppVersion   "1.3.0"
#define MyAppPublisher "9号造物"
#define MyAppExeName   "bs3d_assoc.exe"
#define ThumbClsid     "{E8B5A3F0-2F5E-4C2D-9A34-6C1F8E2B0D77}"
; Must be the IThumbnailProvider IID (Explorer looks the handler up by it).
#define ThumbShellex   "{E357FCCD-A995-4576-B01F-234630154E96}"
; Non-standard GUID written by v1.2.0; swept away so it leaves no cruft.
#define ThumbShellexOld "{E1775614-BA1F-4B66-85BB-33800A1D334F}"

[Setup]
AppId={{7C4E1D2B-93A5-4E8F-B6D0-52FA71C9E3B4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; {autopf} = C:\Program Files (machine install) or
;           %LOCALAPPDATA%\Programs (per-user install)
DefaultDirName={autopf}\Bambu3DAssoc
DefaultGroupName=Bambu3DAssoc
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=Bambu3DAssoc-Setup-{#MyAppVersion}
SetupIconFile=..\resources\app.ico
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Ask the user for the install scope at startup. Choosing "all users" makes
; Setup relaunch itself elevated, so a per-user install never needs UAC.
; "commandline" (implied by "dialog") lets unattended deployments pin the scope:
;   Setup.exe /VERYSILENT /ALLUSERS     -> machine (Program Files + HKLM)
;   Setup.exe /VERYSILENT /CURRENTUSER  -> current user (%LOCALAPPDATA%\Programs + HKCU)
; WARNING: passing neither switch while the dialog is suppressed makes Setup take
; the *recommended* choice, which under PrivilegesRequired=lowest is "all users".
; Scripts must therefore always pass /ALLUSERS or /CURRENTUSER explicitly.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

[Languages]
Name: "chineseb"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Messages]
; The scope picker shipped with ChineseSimplified.isl already says
; 「为所有用户安装 / 仅为我安装」; only the header wording is tightened here.
PrivilegesRequiredOverrideTitle=选择安装范围
PrivilegesRequiredOverrideInstruction=请选择 {#MyAppName} 的安装范围
PrivilegesRequiredOverrideText2=%1 可以仅为你安装（无需管理员权限，装到用户目录，注册表写 HKCU），也可以为所有用户安装（需要管理员权限，装到 Program Files，注册表写 HKLM）。
PrivilegesRequiredOverrideText1=%1 可以为所有用户安装（需要管理员权限，装到 Program Files，注册表写 HKLM），也可以仅为你安装（无需管理员权限，装到用户目录，注册表写 HKCU）。
PrivilegesRequiredOverrideCurrentUser=仅为我安装（单用户，HKCU）(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=仅为我安装（单用户，HKCU）(&M)
PrivilegesRequiredOverrideAllUsers=为所有用户安装（全系统，HKLM）(&A)
PrivilegesRequiredOverrideAllUsersRecommended=为所有用户安装（全系统，HKLM）(&A)

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加图标:"; Flags: unchecked

[Files]
Source: "..\build\dist\bs3d_assoc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\dist\bs3d_thumb.dll"; DestDir: "{app}"; Flags: ignoreversion
; CAD kernel, always bundled. bs3d_occ.dll is loaded from {app}\occt\.
Source: "occt\*"; DestDir: "{app}\occt"; Flags: ignoreversion recursesubdirs createallsubdirs

[Registry]
; Stamp the scope the user picked so the app knows whether to write HKLM or HKCU.
Root: HKLM; Subkey: "SOFTWARE\Bambu3DAssoc"; ValueType: string; ValueName: "InstallScope"; ValueData: "machine"; Flags: uninsdeletekey; Check: IsAdminInstallMode
Root: HKCU; Subkey: "Software\Bambu3DAssoc"; ValueType: string; ValueName: "InstallScope"; ValueData: "user"; Check: not IsAdminInstallMode

[UninstallDelete]
Type: filesandordirs; Name: "{app}\occt"
Type: filesandordirs; Name: "{%LOCALAPPDATA}\Bambu3DAssoc"
Type: filesandordirs; Name: "{commonappdata}\Bambu3DAssoc"

[Icons]
; {group} / {autodesktop} resolve to the per-user locations on a user install.
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Silent auto-apply: runs at the end of every install (including /VERYSILENT,
; hence no skipifsilent). Inherits whatever token Setup has, so on a machine
; install it can write HKLM and on a user install it writes HKCU.
Filename: "{app}\{#MyAppExeName}"; Parameters: "/auto-apply"; Flags: runhidden waituntilterminated
; GUI fallback / fine-tuning entry shown on the finish page.
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}（选择格式并点击应用）"; Flags: postinstall nowait skipifsilent

[UninstallRun]
; Release the thumbnail DLL before the files are removed. Shell extensions are
; mapped into COM surrogates (dllhost.exe / prevhost.exe), and a mapped DLL
; cannot be deleted — without this a silent uninstall leaves bs3d_thumb.dll
; behind. The command only terminates surrogates that actually hold our DLL.
Filename: "{app}\{#MyAppExeName}"; Parameters: "/release-thumb"; Flags: runhidden waituntilterminated; RunOnceId: "ReleaseThumbDll"

[Code]
{ ---------------------------------------------------------------------------
  v1.2.4.  The 1.2.3 uninstaller restored the associations correctly but still
  left four kinds of trace behind, all of them reproduced on a clean Windows
  11 box by uninstalling and then diffing the registry:

    * 30 now-empty ShellEx parents: <ext>\ShellEx and
      SystemFileAssociations\<ext>\ShellEx.  Harmless, but they show up in
      regedit and they are not something we should leave behind.
    * the per-user "open with" records Explorer had stored for our ProgID
      (FileExts\<ext>\OpenWithProgids) and for bs3d_assoc.exe
      (FileExts\<ext>\OpenWithList).  Both become dangling references.
    * the Windows thumbnail cache.  This is the one users actually notice:
      Explorer answers a thumbnail request straight out of thumbcache_*.db
      and never asks whether a handler is still registered, so every .3mf and
      .stl keeps its old preview picture after the uninstall -- which reads
      as "the uninstall did not work".  Deleting the cache is the only way
      to drop those previews.
--------------------------------------------------------------------------- }

{ Raised once the registry is clean so Explorer rebuilds its association and
  icon state right away instead of at the next logon.
  SHCNE_ASSOCCHANGED = $08000000, SHCNF_IDLIST = $0000. }
procedure SHChangeNotifyAssoc(wEventId: Longint; uFlags: Cardinal;
                              dwItem1, dwItem2: Longint);
  external 'SHChangeNotify@shell32.dll stdcall';

var
  { Captured before the uninstall starts: the thumbnail cache only needs
    clearing when our handler was actually registered. }
  g_ThumbWasInstalled: Boolean;

{ True when at least one extension still maps a shell-lookup location to our
  thumbnail CLSID.  Must run BEFORE RestoreOneRoot touches anything. }
function OurThumbnailHandlerRegistered(): Boolean;
var
  exts: array of string;
  roots: array[0..1] of Integer;
  keys: array[0..2] of string;
  i, r, j: Integer;
  v: string;
begin
  Result := False;
  exts := ['3mf','stl','obj','step','stp','fbx','amf','gcode','g','gco'];
  roots[0] := HKEY_LOCAL_MACHINE;
  roots[1] := HKEY_CURRENT_USER;
  for r := 0 to 1 do
  begin
    for i := 0 to GetArrayLength(exts) - 1 do
    begin
      keys[0] := 'Software\Classes\Bambu3D.' + exts[i] +
                 '\ShellEx\{#ThumbShellex}';
      keys[1] := 'Software\Classes\.' + exts[i] +
                 '\ShellEx\{#ThumbShellex}';
      keys[2] := 'Software\Classes\SystemFileAssociations\.' + exts[i] +
                 '\ShellEx\{#ThumbShellex}';
      for j := 0 to 2 do
      begin
        if RegQueryStringValue(roots[r], keys[j], '', v) then
        begin
          if CompareText(v, '{#ThumbClsid}') = 0 then
          begin
            Result := True;
            Exit;
          end;
        end;
      end;
    end;
  end;
end;

{ Explorer caches thumbnails in %LOCALAPPDATA%\Microsoft\Windows\Explorer\
  thumbcache_*.db.  They are opened with FILE_SHARE_DELETE, so they can be
  removed while Explorer is running; Explorer recreates them on demand. }
procedure ClearThumbnailCache();
var
  dir: string;
  FindRec: TFindRec;
  n: Integer;
begin
  dir := ExpandConstant('{localappdata}\Microsoft\Windows\Explorer');
  n := 0;
  if FindFirst(dir + '\thumbcache_*.db', FindRec) then
  begin
    try
      repeat
        if DeleteFile(dir + '\' + FindRec.Name) then
          n := n + 1;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  Log('v1.2.4: removed ' + IntToStr(n) + ' thumbnail cache file(s)');
end;

{ Drops exeName from FileExts\<ext>\OpenWithList and repairs its MRUList, so
  the "open with" menu does not keep offering a program that is gone.  The
  path only exists under HKCU; the HKLM pass exits immediately. }
procedure RemoveOpenWithListEntry(rootKey: Integer; const ext, exeName: string);
var
  key, mru, letter, data, kept: string;
  i: Integer;
begin
  key := 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.' +
         ext + '\OpenWithList';
  if not RegQueryStringValue(rootKey, key, 'MRUList', mru) then Exit;
  kept := '';
  for i := 1 to Length(mru) do
  begin
    letter := Copy(mru, i, 1);
    if RegQueryStringValue(rootKey, key, letter, data) then
    begin
      if CompareText(data, exeName) = 0 then
        RegDeleteValue(rootKey, key, letter)
      else
        kept := kept + letter;
    end
    else
      kept := kept + letter;
  end;
  if kept <> mru then
  begin
    if kept = '' then
      RegDeleteValue(rootKey, key, 'MRUList')
    else
      RegWriteStringValue(rootKey, key, 'MRUList', kept);
  end;
end;

procedure RestoreOneRoot(const rootName, ext: string);
var
  rootKey: Integer;
  classes, extPath, progId, saved: string;
begin
  if rootName = 'HKLM' then
    rootKey := HKEY_LOCAL_MACHINE
  else
    rootKey := HKEY_CURRENT_USER;

  classes  := 'Software\Classes';
  extPath  := classes + '\.' + ext;
  progId   := 'Bambu3D.' + ext;

  { remove our thumbnail handler from every shell-lookup location, both the
    current IID and the non-standard GUID written by v1.2.0 }
  RegDeleteKeyIncludingSubkeys(rootKey, extPath + '\ShellEx\{#ThumbShellex}');
  RegDeleteKeyIncludingSubkeys(rootKey, extPath + '\ShellEx\{#ThumbShellexOld}');
  RegDeleteKeyIncludingSubkeys(rootKey,
    classes + '\SystemFileAssociations\.' + ext + '\ShellEx\{#ThumbShellex}');
  RegDeleteKeyIncludingSubkeys(rootKey,
    classes + '\SystemFileAssociations\.' + ext + '\ShellEx\{#ThumbShellexOld}');

  { restore the original default handler if we saved one }
  if RegQueryStringValue(rootKey, extPath, '', saved) then
  begin
    if saved = progId then
    begin
      if RegQueryStringValue(rootKey, extPath + '\ShellEx\SavedHandler', '', saved)
         and (saved <> '') then
        RegWriteStringValue(rootKey, extPath, '', saved)
      else
        RegDeleteValue(rootKey, extPath, '');
      RegDeleteKeyIncludingSubkeys(rootKey, extPath + '\ShellEx\SavedHandler');
    end;
  end;

  { v1.2.4: removing the handler value leaves the ShellEx parent behind.
    Delete it once it is empty -- if a third party owns a handler there it
    still has a subkey and RegDeleteKeyIfEmpty declines. }
  RegDeleteKeyIfEmpty(rootKey, extPath + '\ShellEx');
  RegDeleteKeyIfEmpty(rootKey,
    classes + '\SystemFileAssociations\.' + ext + '\ShellEx');
  RegDeleteKeyIfEmpty(rootKey,
    classes + '\SystemFileAssociations\.' + ext);

  { v1.2.4: Explorer recorded our ProgID and our exe in the per-user "open
    with" lists.  Both dangle once the app is gone. }
  RegDeleteValue(rootKey,
    'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.' + ext +
    '\OpenWithProgids', progId);
  RemoveOpenWithListEntry(rootKey, ext, '{#MyAppExeName}');

  { The ProgID is ours by naming, so it goes unconditionally — and it must go
    *after* the default value above was handed back. Since v1.2.0 no longer
    takes over the default handler, this branch is the normal case and the key
    would otherwise survive as an empty leftover. }
  RegDeleteKeyIncludingSubkeys(rootKey, classes + '\' + progId);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  exts: array of string;
  i: Integer;
  roots: array[0..1] of string;
  r: Integer;
begin
  { Capture this BEFORE anything is torn down: the thumbnail cache only has
    to be cleared if we actually registered a handler for some extension. }
  if CurUninstallStep = usUninstall then
  begin
    g_ThumbWasInstalled := OurThumbnailHandlerRegistered();
    if g_ThumbWasInstalled then
      Log('v1.2.4: thumbnail handler was registered; cache will be cleared')
    else
      Log('v1.2.4: no thumbnail handler registered; cache left alone');
    Exit;
  end;

  if CurUninstallStep <> usPostUninstall then Exit;

  { Union of the current 1.1.0 list (3mf stl obj amf fbx step stp gcode g gco)
    and the 10 legacy extensions from 1.0.0, so leftovers from older versions
    are always restored. RestoreOneRoot only acts on extensions whose default
    handler is actually ours, extra entries are no-ops. }
  exts := ['3mf','stl','obj','amf','fbx','step','stp','gcode','g','gco',
           'ply','3ds','off','dae','x3d','wrl','iges','igs','brep','bgcode'];
  roots[0] := 'HKLM';
  roots[1] := 'HKCU';

  for r := 0 to 1 do
  begin
    for i := 0 to GetArrayLength(exts) - 1 do
      RestoreOneRoot(roots[r], exts[i]);

    { a per-user Interface entry is ours alone; the HKLM one is Windows' own
      standard ProxyStubClsid32 registration for the same shell IIDs }
    if roots[r] = 'HKCU' then
    begin
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER,
        'Software\Classes\Interface\{e357fccd-a995-4576-b01f-234630154e96}');
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER,
        'Software\Classes\Interface\{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}');
    end;

    { COM class registration + interface stubs + app settings }
    if roots[r] = 'HKLM' then
    begin
      RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE,
        'SOFTWARE\Classes\CLSID\{#ThumbClsid}');
      RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'SOFTWARE\Bambu3DAssoc');
    end
    else
    begin
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER,
        'Software\Classes\CLSID\{#ThumbClsid}');
      RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Bambu3DAssoc');
    end;
  end;

  { v1.2.4: Explorer answers thumbnail requests straight out of
    thumbcache_*.db without re-checking whether a handler still exists, so
    every .3mf/.stl preview would outlive the uninstall.  Clear the cache --
    but only when we were the ones who put thumbnails there. }
  if g_ThumbWasInstalled then
    ClearThumbnailCache();

  { ask the shell to reload association / icon state }
  SHChangeNotifyAssoc($08000000, $0000, 0, 0);
end;
