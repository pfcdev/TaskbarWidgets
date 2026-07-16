Unicode true
RequestExecutionLevel user

!ifndef VERSION
  !error "VERSION must be provided"
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
Var LegacyInstallDir
Var LegacyUpgrade

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
  StrCpy $LegacyUpgrade "0"
  ${If} $EXEFILE == "TaskbarStatsSetup.exe"
    StrCpy $LegacyUpgrade "1"
  ${EndIf}
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
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.WidgetHost.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.Settings.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarStats.exe /F /T'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarStats"
FunctionEnd

Function DisableLegacyLoader
  ReadRegStr $LegacyInstallDir HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarStats" "InstallLocation"
  ${If} $LegacyInstallDir == ""
    StrCpy $LegacyInstallDir "$LOCALAPPDATA\Programs\TaskbarStats"
  ${EndIf}

  ; Preserve legacy user data and the old binary, but move the loader away
  ; from the exact path that the <=0.2.7 updater tries to restart.
  ${If} $LegacyInstallDir != "$INSTDIR"
    IfFileExists "$LegacyInstallDir\TaskbarStats.exe" 0 legacy_loader_done
    Delete "$LegacyInstallDir\TaskbarStats.exe.migrated"
    Rename "$LegacyInstallDir\TaskbarStats.exe" "$LegacyInstallDir\TaskbarStats.exe.migrated"
  ${EndIf}
legacy_loader_done:
FunctionEnd

Section "Taskbar Widgets" SecMain
  SectionIn RO
  Call StopTaskbarWidgets
  SetOutPath "$INSTDIR"
  File /r "${PACKAGE_ROOT}\*.*"
  Call DisableLegacyLoader
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

  ; Double-clicking a .twidget always opens the Settings permission review.
  WriteRegStr HKCU "Software\Classes\.twidget" "" "TaskbarWidgets.WidgetPackage"
  WriteRegStr HKCU "Software\Classes\.twidget\OpenWithProgids" "TaskbarWidgets.WidgetPackage" ""
  WriteRegStr HKCU "Software\Classes\TaskbarWidgets.WidgetPackage" "" "Taskbar Widgets Package"
  WriteRegStr HKCU "Software\Classes\TaskbarWidgets.WidgetPackage\DefaultIcon" "" "$INSTDIR\TaskbarWidgets.exe,0"
  WriteRegStr HKCU "Software\Classes\TaskbarWidgets.WidgetPackage\shell\open\command" "" '$\"$INSTDIR\TaskbarWidgets.exe$\" --install-widget $\"%1$\"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ; A silent legacy updater has no finish page and its old apply script only
  ; knows how to restart TaskbarStats.exe. Start the new loader here instead.
  ${If} $LegacyUpgrade == "1"
    IfSilent 0 legacy_upgrade_done
    Exec '"$INSTDIR\TaskbarWidgets.exe" --no-update-check'
  ${EndIf}
legacy_upgrade_done:
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
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.WidgetHost.exe /F /T'
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /IM TaskbarWidgets.Settings.exe /F /T'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TaskbarWidgets"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TaskbarWidgets"
  DeleteRegKey HKCU "Software\Classes\TaskbarWidgets.WidgetPackage"
  DeleteRegValue HKCU "Software\Classes\.twidget\OpenWithProgids" "TaskbarWidgets.WidgetPackage"
  DeleteRegKey /ifempty HKCU "Software\Classes\.twidget\OpenWithProgids"
  DeleteRegKey /ifempty HKCU "Software\Classes\.twidget"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  Delete "$DESKTOP\Taskbar Widgets.lnk"
  RMDir /r "$SMPROGRAMS\Taskbar Widgets"

  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.exe"
  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.Settings.exe"
  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.MediaHelper.exe"
  Delete /REBOOTOK "$INSTDIR\TaskbarWidgets.WidgetHost.exe"
  Delete /REBOOTOK "$INSTDIR\twdev.exe"
  Delete /REBOOTOK "$INSTDIR\README-PORTABLE.txt"
  Delete /REBOOTOK "$INSTDIR\Uninstall Taskbar Widgets.exe"
  RMDir /r /REBOOTOK "$INSTDIR\Assets"
  RMDir /r /REBOOTOK "$INSTDIR\Widgets"
  RMDir /r /REBOOTOK "$INSTDIR\CommunitySDK"
  ${If} $RemoveUserData == "1"
    RMDir /r /REBOOTOK "$INSTDIR\Data"
  ${EndIf}
  RMDir /REBOOTOK "$INSTDIR"
SectionEnd
