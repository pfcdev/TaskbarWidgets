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

Name "Taskbar Widgets"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\TaskbarWidgets"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "InstallLocation"
SetCompressor /SOLID lzma
BrandingText "Taskbar Widgets ${VERSION}"

!if "${ICON_FILE}" != ""
  Icon "${ICON_FILE}"
  UninstallIcon "${ICON_FILE}"
!endif

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install Taskbar Widgets"
!define MUI_WELCOMEPAGE_TEXT "This wizard installs Taskbar Widgets for the current Windows user.$\r$\n$\r$\nExisting TaskbarStats settings are migrated once and are never deleted automatically."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder in which to install Taskbar Widgets. Application data will be stored in its Data subfolder."
!define MUI_FINISHPAGE_RUN "$INSTDIR\TaskbarWidgets.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Taskbar Widgets now"

Var StartWithWindows
Var DesktopShortcut
Var StartWithWindowsCheckbox
Var DesktopShortcutCheckbox
Var RemoveUserData
Var RemoveUserDataCheckbox

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom OptionsPage OptionsPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
UninstPage custom un.DataPage un.DataPageLeave
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  StrCpy $StartWithWindows "1"
  StrCpy $DesktopShortcut "0"
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "InstallLocation"
  ${If} $0 != ""
    ReadRegStr $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarWidgets"
    ${If} $1 == ""
      StrCpy $StartWithWindows "0"
    ${EndIf}
    IfFileExists "$DESKTOP\Taskbar Widgets.lnk" 0 +2
      StrCpy $DesktopShortcut "1"
  ${EndIf}
FunctionEnd

Function OptionsPage
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 20u "Choose shortcuts and startup behavior."
  Pop $0
  ${NSD_CreateCheckbox} 0 30u 100% 12u "Run Taskbar Widgets when Windows starts"
  Pop $StartWithWindowsCheckbox
  ${If} $StartWithWindows == "1"
    ${NSD_Check} $StartWithWindowsCheckbox
  ${EndIf}
  ${NSD_CreateCheckbox} 0 52u 100% 12u "Create a desktop shortcut"
  Pop $DesktopShortcutCheckbox
  ${If} $DesktopShortcut == "1"
    ${NSD_Check} $DesktopShortcutCheckbox
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
  ${NSD_GetState} $DesktopShortcutCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $DesktopShortcut "1"
  ${Else}
    StrCpy $DesktopShortcut "0"
  ${EndIf}
FunctionEnd

Function StopTaskbarWidgets
  IfFileExists "$INSTDIR\TaskbarWidgets.exe" 0 +2
    ExecWait '"$INSTDIR\TaskbarWidgets.exe" --detach'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.MediaHelper.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.Settings.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStats.exe /F /T'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats"
FunctionEnd

Section "Taskbar Widgets" SecMain
  SectionIn RO
  Call StopTaskbarWidgets
  SetOutPath "$INSTDIR"
  File /r "${PACKAGE_ROOT}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall Taskbar Widgets.exe"

  CreateDirectory "$SMPROGRAMS\Taskbar Widgets"
  CreateShortcut "$SMPROGRAMS\Taskbar Widgets\Taskbar Widgets.lnk" "$INSTDIR\TaskbarWidgets.exe"
  CreateShortcut "$SMPROGRAMS\Taskbar Widgets\Taskbar Widgets Settings.lnk" "$INSTDIR\TaskbarWidgets.exe" "--settings"
  CreateShortcut "$SMPROGRAMS\Taskbar Widgets\Uninstall Taskbar Widgets.lnk" "$INSTDIR\Uninstall Taskbar Widgets.exe"

  ${If} $DesktopShortcut == "1"
    CreateShortcut "$DESKTOP\Taskbar Widgets.lnk" "$INSTDIR\TaskbarWidgets.exe"
  ${Else}
    Delete "$DESKTOP\Taskbar Widgets.lnk"
  ${EndIf}
  ${If} $StartWithWindows == "1"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarWidgets" '"$INSTDIR\TaskbarWidgets.exe"'
  ${Else}
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarWidgets"
  ${EndIf}

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "DisplayName" "Taskbar Widgets"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "Publisher" "PFC"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "DisplayIcon" "$INSTDIR\TaskbarWidgets.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "UninstallString" '"$INSTDIR\Uninstall Taskbar Widgets.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "QuietUninstallString" '"$INSTDIR\Uninstall Taskbar Widgets.exe" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets" "NoRepair" 1
SectionEnd

Function un.DataPage
  StrCpy $RemoveUserData "0"
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 28u "Taskbar Widgets can keep your settings for a later installation."
  Pop $0
  ${NSD_CreateCheckbox} 0 36u 100% 12u "Also remove settings, accounts, logs and cached data"
  Pop $RemoveUserDataCheckbox
  nsDialogs::Show
FunctionEnd

Function un.DataPageLeave
  ${NSD_GetState} $RemoveUserDataCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $RemoveUserData "1"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.MediaHelper.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.Settings.exe /F /T'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarWidgets"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets"
  Delete "$DESKTOP\Taskbar Widgets.lnk"
  RMDir /r "$SMPROGRAMS\Taskbar Widgets"

  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.exe"
  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.Settings.exe"
  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.MediaHelper.exe"
  Delete /REBOOTOK "$INSTDIR\README-PORTABLE.txt"
  Delete /REBOOTOK "$INSTDIR\Uninstall Taskbar Widgets.exe"
  RMDir /r /REBOOTOK "$INSTDIR\Assets"
  RMDir /r /REBOOTOK "$INSTDIR\Widgets"
  ${If} $RemoveUserData == "1"
    RMDir /r /REBOOTOK "$INSTDIR\Data"
  ${EndIf}
  RMDir /REBOOTOK "$INSTDIR"
SectionEnd
