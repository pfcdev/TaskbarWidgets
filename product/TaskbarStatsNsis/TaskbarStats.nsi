Unicode true
RequestExecutionLevel user

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef PACKAGE_ROOT
  !error "PACKAGE_ROOT must be provided"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE must be provided"
!endif
!ifndef ICON_FILE
  !define ICON_FILE ""
!endif

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"

Name "TaskbarStats"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\TaskbarStats"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "InstallLocation"
SetCompressor /SOLID lzma
BrandingText "TaskbarStats ${VERSION}"

!if "${ICON_FILE}" != ""
  Icon "${ICON_FILE}"
  UninstallIcon "${ICON_FILE}"
!endif

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install TaskbarStats"
!define MUI_WELCOMEPAGE_TEXT "This wizard will install TaskbarStats on your computer.$\r$\n$\r$\nIt will update an existing installation in place."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install TaskbarStats."
!define MUI_FINISHPAGE_RUN "$INSTDIR\TaskbarStats.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start TaskbarStats now"

Var StartWithWindows
Var StartWithWindowsCheckbox

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom OptionsPage OptionsPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  StrCpy $StartWithWindows "1"
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "InstallLocation"
  ${If} $0 != ""
    ReadRegStr $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats"
    ${If} $1 == ""
      StrCpy $StartWithWindows "0"
    ${EndIf}
  ${EndIf}
FunctionEnd

Function OptionsPage
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 20u "Choose startup behavior."
  Pop $0

  ${NSD_CreateCheckbox} 0 30u 100% 12u "Run TaskbarStats when Windows starts"
  Pop $StartWithWindowsCheckbox
  ${If} $StartWithWindows == "1"
    ${NSD_Check} $StartWithWindowsCheckbox
  ${EndIf}

  nsDialogs::Show
FunctionEnd

Function OptionsPageLeave
  ${NSD_GetState} $StartWithWindowsCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $StartWithWindows "1"
  ${Else}
    StrCpy $StartWithWindows "0"
  ${EndIf}
FunctionEnd

Function StopTaskbarStats
  IfFileExists "$INSTDIR\TaskbarStats.exe" 0 +2
    ExecWait '"$INSTDIR\TaskbarStats.exe" --detach'
  Sleep 1500
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStats.exe /F'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStatsMediaHelper.exe /F'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStatsSettings.exe /F'
FunctionEnd

Section "TaskbarStats" SecMain
  SectionIn RO

  Call StopTaskbarStats

  SetOutPath "$INSTDIR"
  File /r "${PACKAGE_ROOT}\*.*"

  WriteUninstaller "$INSTDIR\Uninstall TaskbarStats.exe"

  CreateDirectory "$SMPROGRAMS\TaskbarStats"
  CreateShortcut "$SMPROGRAMS\TaskbarStats\TaskbarStats.lnk" "$INSTDIR\TaskbarStats.exe" "" "$INSTDIR\TaskbarStats.exe"
  CreateShortcut "$SMPROGRAMS\TaskbarStats\TaskbarStats Settings.lnk" "$INSTDIR\TaskbarStatsSettings.exe" "" "$INSTDIR\TaskbarStatsSettings.exe"
  CreateShortcut "$SMPROGRAMS\TaskbarStats\Uninstall TaskbarStats.lnk" "$INSTDIR\Uninstall TaskbarStats.exe"

  ${If} $StartWithWindows == "1"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats" '"$INSTDIR\TaskbarStats.exe"'
  ${Else}
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats"
  ${EndIf}

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "DisplayName" "TaskbarStats"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "Publisher" "TaskbarStats"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "DisplayIcon" "$INSTDIR\TaskbarStats.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "UninstallString" '"$INSTDIR\Uninstall TaskbarStats.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "QuietUninstallString" '"$INSTDIR\Uninstall TaskbarStats.exe" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Call un.StopTaskbarStats

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats"

  Delete "$SMPROGRAMS\TaskbarStats\TaskbarStats.lnk"
  Delete "$SMPROGRAMS\TaskbarStats\TaskbarStats Settings.lnk"
  Delete "$SMPROGRAMS\TaskbarStats\Uninstall TaskbarStats.lnk"
  RMDir "$SMPROGRAMS\TaskbarStats"

  Delete /REBOOTOK "$INSTDIR\Uninstall TaskbarStats.exe"
  RMDir /r /REBOOTOK "$INSTDIR"
  RMDir /r /REBOOTOK "$LOCALAPPDATA\TaskbarStats"
  RMDir /r /REBOOTOK "$LOCALAPPDATA\Programs\TaskbarStats"

  Call un.StartExplorer
SectionEnd

Function un.StopTaskbarStats
  IfFileExists "$INSTDIR\TaskbarStats.exe" 0 +2
    ExecWait '"$INSTDIR\TaskbarStats.exe" --detach'
  Sleep 1500
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStats.exe /F'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStatsMediaHelper.exe /F'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStatsSettings.exe /F'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM explorer.exe /F'
  Sleep 1000
FunctionEnd

Function un.StartExplorer
  Exec '"$WINDIR\explorer.exe"'
FunctionEnd
