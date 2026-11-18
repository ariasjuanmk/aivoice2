; Instalador de AI Voice Converter (Inno Setup 6). Solo ASCII a proposito.
; Compilar:  ISCC.exe installer\AIVoiceConverter.iss [/DAppVersion=0.1.0]
; Requiere que exista ..\dist\AI Voice Converter.vst3 (lo deja el workflow de build).

#define AppName "AI Voice Converter"
#ifndef AppVersion
  #define AppVersion "0.2.2"
#endif

[Setup]
AppId={{7C2E81B6-4D93-4A5F-B1E0-9A6D3C58F274}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=StemTools
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir=Output
OutputBaseFilename=AIVoiceConverter-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
SetupLogging=yes

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "engine"; Description: "Instalar ahora el motor de IA (Python + PyTorch + RVC). Descarga varios GB, recomendado"; GroupDescription: "Motor de IA:"
Name: "cpuonly"; Description: "Usar solo CPU (no instalar PyTorch con CUDA)"; GroupDescription: "Motor de IA:"; Flags: unchecked

[Files]
; el plugin va a la carpeta VST3 estandar que escanean todos los DAWs
Source: "..\dist\AI Voice Converter.vst3\*"; DestDir: "{commoncf64}\VST3\AI Voice Converter.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "install_engine.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\engine\rvc_engine.py"; DestDir: "{app}"; Flags: ignoreversion
; instaladores incluidos (los baja el workflow de build)
Source: "redist\python-installer.exe"; DestDir: "{app}\redist"; Flags: ignoreversion skipifsourcedoesntexist
Source: "redist\vs_BuildTools.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall skipifsourcedoesntexist

[Run]
; fairseq (lo usa RVC) no trae version precompilada para Windows: hace falta el compilador de C++ de Microsoft.
; Solo se instala si no esta. Descarga ~2-3 GB y tarda varios minutos.
Filename: "{tmp}\vs_BuildTools.exe"; Parameters: "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"; StatusMsg: "Instalando compilador de C++ (Build Tools, varios minutos, descarga ~3 GB)..."; Flags: waituntilterminated skipifdoesntexist; Check: NeedsBuildTools
; Se ejecuta como el usuario que lanzo el instalador (no como administrador), para que
; el motor quede en SU %LOCALAPPDATA%, que es donde lo busca el plugin.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install_engine.ps1"" -EngineScript ""{app}\rvc_engine.py"" -PythonInstaller ""{app}\redist\python-installer.exe"""; StatusMsg: "Instalando el motor de IA (puede tardar varios minutos)..."; Flags: runasoriginaluser waituntilterminated; Tasks: engine and not cpuonly
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install_engine.ps1"" -EngineScript ""{app}\rvc_engine.py"" -PythonInstaller ""{app}\redist\python-installer.exe"" -CpuOnly"; StatusMsg: "Instalando el motor de IA (CPU)..."; Flags: runasoriginaluser waituntilterminated; Tasks: engine and cpuonly

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
function NeedsBuildTools(): Boolean;
var
  VsWhere, OutFile: String;
  Content: AnsiString;
  ResultCode: Integer;
begin
  Result := True;
  VsWhere := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(VsWhere) then
    Exit;
  OutFile := ExpandConstant('{tmp}\vswhere.txt');
  DeleteFile(OutFile);
  // vswhere sale con codigo 0 aunque no encuentre nada: se mira si escribio una ruta
  Exec(ExpandConstant('{cmd}'),
       '/C ""' + VsWhere + '" -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "' + OutFile + '""',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if LoadStringFromFile(OutFile, Content) then
    Result := (Length(Trim(String(Content))) = 0);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if MsgBox('Borrar tambien el motor de IA, tus modelos de voz copiados y la cache?' + #13#10 +
              '(ocupan varios GB, en ' + ExpandConstant('{localappdata}\AIVoiceConverter') + ')',
              mbConfirmation, MB_YESNO) = IDYES then
      DelTree(ExpandConstant('{localappdata}\AIVoiceConverter'), True, True, True);
  end;
end;
