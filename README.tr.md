# Taskbar Widgets - Hızlı Başlangıç

Taskbar Widgets, Windows 11 x64 görev çubuğuna canlı bilgi kartları ekleyen açık
kaynak bir uygulamadır. İlk sürüm Codex, hava durumu, Discord Voice, medya ve
Steam indirme widget'larını içerir. Widget'lar yan yana veya isteğe bağlı
rotasyon modunda gösterilebilir.

## Kurulum

GitHub Releases sayfasındaki `TaskbarWidgetsSetup-x64.exe` dosyasını çalıştırın.
Sihirbaz kurulum konumu, Windows ile başlatma, masaüstü kısayolu ve kurulumdan
sonra başlatma seçeneklerini gösterir. İmzasız beta yayınlarda Windows
SmartScreen uyarısı görülebilir.

Ayarları ana uygulamadan açın:

```powershell
TaskbarWidgets.exe --settings
```

Kullanıcı verileri varsayılan olarak
`%LOCALAPPDATA%\Programs\TaskbarWidgets\Data` altında tutulur. Kaldırıcı bu
verileri varsayılan olarak korur; silmek için kaldırma ekranındaki ayrı seçeneği
işaretlemek gerekir.

## Kaynaktan doğrulama

.NET 8 SDK, Rust stable, Visual Studio 2022 C++ Build Tools/Windows SDK ve CMake
kurulduktan sonra:

```powershell
.\build.ps1 -Target Verify
.\build.ps1 -Target Build
```

Paket üretmek için NSIS de gerekir:

```powershell
.\build.ps1 -Target Package -InstallDependencies
```

Ayrıntılar için [build rehberine](docs/building.md) ve
[mimari dokümanına](docs/architecture.md) bakın.
