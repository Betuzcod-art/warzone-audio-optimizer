[Version]
Class=IEXPRESS
SEDVersion=3

[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=1
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=I
InstallPrompt=Please run this installer as administrator.
DisplayPrompt=This package installs Warzone Audio Optimizer and VB-CABLE.
FinishMessage=Installation completed. Restart Windows after installing VB-CABLE.
TargetName=WarzoneAudioOptimizer-Setup.exe
FriendlyName=Warzone Audio Optimizer Setup
AppLaunched=install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=install.cmd
UserAccount=Admin
SourceFiles=SourceFiles

; IExpress exige una ruta ABSOLUTA aqui -- no admite rutas relativas al
; propio .sed. Antes de compilar el instalador (iexpress /N /Q
; WarzoneAudioOptimizer.sed), sustituye la linea de abajo por la ruta
; absoluta real de tu checkout local, ej:
;   SourceFiles0=C:\ruta\a\tu\checkout\installer\package
[SourceFiles]
SourceFiles0=RUTA_ABSOLUTA_A_TU_CHECKOUT\installer\package

[SourceFiles0]
%FILE0%=
%FILE1%=
%FILE2%=
%FILE3%=

[Strings]
FILE0=install.cmd
FILE1=uninstall.cmd
FILE2=README.txt
FILE3=payload.zip
