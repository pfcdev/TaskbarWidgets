<p align="center">
  <img src="assets/branding/logo.png" alt="Taskbar Widgets logosu" width="112" height="112" />
</p>

<h1 align="center">Taskbar Widgets</h1>

<p align="center">
  Windows 11 görev çubuğunda doğal görünen, canlı ve kullanışlı widget'lar.
</p>

<p align="center">
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest"><img alt="Son sürüm" src="https://img.shields.io/github/v/release/pfcdev/TaskbarWidgets?sort=semver&display_name=tag&style=flat-square" /></a>
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases"><img alt="Toplam sürüm indirmeleri" src="https://img.shields.io/github/downloads/pfcdev/TaskbarWidgets/total?style=flat-square&label=downloads&color=8B5CF6" /></a>
  <img alt="Windows 11 x64" src="https://img.shields.io/badge/Windows%2011-x64-0078D4?style=flat-square&logo=windows11&logoColor=white" />
  <a href="LICENSE"><img alt="MIT Lisansı" src="https://img.shields.io/badge/license-MIT-22c55e?style=flat-square" /></a>
</p>

<p align="center">
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgetsSetup-x64.exe"><img alt="Son installer sürümünü indir" src="https://img.shields.io/badge/İNDİR-SON%20INSTALLER-2563EB?style=for-the-badge&logo=windows11&logoColor=white" /></a>
</p>

<p align="center">
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest">Sürüm notları</a>
  ·
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgets-portable-x64.zip">Portable ZIP</a>
  ·
  <a href="README.md">English</a>
</p>

<p align="center">
  <img src="sf.gif" alt="Windows 11 görev çubuğunda çalışan Taskbar Widgets" />
</p>

Taskbar Widgets, **Windows 11 x64** görev çubuğuna canlı bilgiler ve kullanışlı
kontroller ekleyen ücretsiz, açık kaynaklı bir uygulamadır. İstediğiniz
widget'ları seçebilir, doğrudan görev çubuğunda sürükleyebilir ve hepsini tek bir
Settings uygulamasından yönetebilirsiniz.

> [!IMPORTANT]
> Taskbar Widgets beta aşamasındadır ve Windows 11'in özel XAML yüzeyleriyle
> çalışır. Bir Windows güncellemesi uyumluluğu geçici olarak etkileyebilir.
> Desteklenmeyen bir görev çubuğu algılanırsa uygulama riskli bir yerleşimi
> zorlamak yerine entegrasyonu güvenli şekilde kapatır.

## Nasıl görünüyor?

<table>
  <tr>
    <td align="center">
      <img src="assets/readme/widget-gallery/collage-productivity.png" alt="Codex Status, Discord Voice ve Parking Lot widget'ları" /><br />
      <strong>Çalışma ve iletişim</strong>
    </td>
    <td align="center">
      <img src="assets/readme/widget-gallery/collage-media-weather.png" alt="Hava durumu ve medya widget'ları" /><br />
      <strong>Hava durumu ve medya</strong>
    </td>
  </tr>
  <tr>
    <td align="center">
      <img src="assets/readme/widget-gallery/collage-system-monitoring.png" alt="CPU, bellek, depolama ve ağ widget'ları" /><br />
      <strong>Canlı sistem takibi</strong>
    </td>
    <td align="center">
      <img src="assets/readme/widget-gallery/collage-utilities.png" alt="Steam Downloads ve Parking Lot widget'ları" /><br />
      <strong>İndirmeler ve hızlı dosya park etme</strong>
    </td>
  </tr>
</table>

## Kurulum

1. [Son installer sürümünü indirin](https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgetsSetup-x64.exe).
2. Installer'ı çalıştırın ve son ekranda **Start Taskbar Widgets** seçeneğini açık bırakın.
3. Bildirim alanındaki Taskbar Widgets ikonundan **Open Settings** seçeneğine basın.
4. İstediğiniz widget'ları etkinleştirin ve görev çubuğunda sürükleyerek yerleştirin.

Kurulumda seçerseniz uygulama Windows ile birlikte başlar. Bildirim alanı
menüsünden Settings'i açabilir veya tüm widget'ları hızlıca açıp kapatabilirsiniz.

> [!NOTE]
> İmzasız beta sürümlerde Windows SmartScreen uyarısı çıkabilir. İndirdiğiniz
> installer'ı sürüm sayfasındaki SHA-256 dosyasıyla doğrulayabilirsiniz.

## Dahili widget'lar

- **Weather:** Konum, sıcaklık ve güncel hava koşulları.
- **Steam Downloads:** Oyun adı, indirme ilerlemesi, hız ve boyut.
- **Codex Status:** Aktif iş durumu, kota bilgileri ve yerel hesap kontrolleri.
- **Discord Voice:** Ses odası, katılımcılar, susturma durumu ve konuşan kişi halkası.
- **Media Player:** Aktif Windows medya oturumu, kapak görseli ve oynat/duraklat.
- **Parking Lot:** Dosya, klasör, bağlantı veya metni geçici olarak görev çubuğunda tutma.
- **CPU:** Toplam veya çekirdek başına kullanım.
- **Memory:** Fiziksel bellek kullanımı.
- **Storage:** Disk okuma ve yazma hızı.
- **Network:** Canlı indirme ve yükleme hızı.

<p align="center">
  <img src="docs/images/settings-library.png" alt="Taskbar Widgets Widget Library ekranı" />
</p>

Sistem sayaçlarında metin, bar ve pasta görünümleri; özel renkler ve 0,1-10
saniye arası yenileme seçenekleri bulunur. Her widget bağımsız olarak açılıp
kapatılabilir ve konumlandırılabilir.

## Öne çıkan özellikler

- Birden fazla widget'ı yan yana gösterme veya sırayla döndürme.
- Widget'ları doğrudan görev çubuğunda sürükleyerek konumlandırma.
- Widget'lar ve görev çubuğu ikonları için çarpışma koruması.
- Bildirim alanı ikonundan Settings ve genel aç/kapat kontrolü.
- Explorer yeniden başladığında otomatik toparlanma.
- Yerel Settings arayüzü ve otomatik güncelleme desteği.
- İzinleri kurulumdan önce gösterilen Community widget desteği.

## Discord'a müdahale etmeden sesli oda algılama

Discord Voice, normal şekilde çalışan Discord masaüstü penceresinden seçili ses
odasını ve katılımcı bilgilerini okur. Discord'a kod eklemez, uygulamayı
değiştirmez, bot veya OAuth girişi istemez.

Daha hızlı konuşma halkaları için Settings üzerinden isteğe bağlı **Instant
Speaking Detection** yardımcısını kurabilirsiniz. Windows yalnızca yardımcı
kurulurken veya kaldırılırken yönetici izni ister. Yardımcı, konuşmanın başlama
ve bitiş zamanını algılar; görüşme sesini dinlemez, kaydetmez veya saklamaz.

## Parking Lot

Parking Lot görev çubuğunda küçük bir sürükle-bırak alanıdır. Bir dosya, klasör,
bağlantı veya metni üzerine bırakabilir; daha sonra widget'tan başka bir klasöre
ya da uygulamaya sürükleyebilirsiniz. Sağ tıklayarak seçili öğeyi kaldırabilir
veya tüm alanı temizleyebilirsiniz.

İçerikler internete yüklenmez; referanslar yalnızca bilgisayarınızda tutulur.

## Medyaya göre değişen renkler

Media Player arka planını, vurgu rengini ve kontrollerini aktif kapak görseline
göre otomatik uyarlar:

<p align="center">
  <img src="assets/readme/media-dynamic/media-palette-01.gif" alt="Dinamik medya rengi 1" width="280" />
  <img src="assets/readme/media-dynamic/media-palette-02.gif" alt="Dinamik medya rengi 2" width="280" />
  <img src="assets/readme/media-dynamic/media-palette-03.gif" alt="Dinamik medya rengi 3" width="280" />
  <br />
  <img src="assets/readme/media-dynamic/media-palette-04.gif" alt="Dinamik medya rengi 4" width="280" />
  <img src="assets/readme/media-dynamic/media-palette-05.gif" alt="Dinamik medya rengi 5" width="280" />
</p>

## İndirme seçenekleri

| Paket | Kullanım | İndir |
| --- | --- | --- |
| Installer | Önerilen kurulum, başlangıç entegrasyonu ve güncellemeler | [TaskbarWidgetsSetup-x64.exe](https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgetsSetup-x64.exe) |
| Portable ZIP | Manuel ve bağımsız kullanım | [TaskbarWidgets-portable-x64.zip](https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgets-portable-x64.zip) |
| Sürüm sayfası | Sürüm notları, checksum ve tüm dosyalar | [Son GitHub sürümü](https://github.com/pfcdev/TaskbarWidgets/releases/latest) |

Varsayılan kurulum konumu:

```text
%LOCALAPPDATA%\Programs\TaskbarWidgets
```

Kaldırıcı, **Also remove settings and data** seçeneği işaretlenmedikçe ayarları
ve widget verilerini korur.

## Veri ve gizlilik

Ayarlar, widget yapılandırması ve çalışma durumu bilgisayarınızda şu klasörde
tutulur:

```text
%LOCALAPPDATA%\Programs\TaskbarWidgets\Data
```

Hava durumu, sürüm kontrolü ve medya kapakları gibi bazı özellikler kendi veri
kaynaklarına bağlanabilir. Sağlayıcı işlemleri Explorer dışında çalışır; bir
entegrasyonun hata vermesi diğer widget'ları durdurmaz.

## Sorun giderme

- **Kurulumdan sonra widget görünmüyor:** Başlat menüsünden Taskbar Widgets'ı çalıştırın ve bildirim alanı ikonunu kontrol edin. Gerekirse Explorer'ı bir kez yeniden başlatın.
- **Belirli bir widget görünmüyor:** Settings üzerinden etkin olduğunu doğrulayın.
- **Settings açılmıyor:** Kurulum klasöründe `TaskbarWidgets.exe --settings` komutunu çalıştırın.
- **Discord konuşma halkası geç güncelleniyor:** Discord Voice ayarlarından **Instant Speaking Detection** özelliğini etkinleştirin.
- **SmartScreen uyarısı çıkıyor:** Devam etmeden önce SHA-256 değerini sürümde yayınlanan dosyayla karşılaştırın.

Daha fazla çözüm için [sorun giderme rehberine](docs/troubleshooting.md) bakın.

## Geliştiriciler

Kaynaktan derleme, mimari ve widget geliştirme bilgileri:

- [Build rehberi](docs/building.md)
- [Mimari](docs/architecture.md)
- [Widget protokolü](docs/protocol.md)
- [Community SDK](community-sdk/README.md)
- [Katkıda bulunma rehberi](CONTRIBUTING.md)

Güvenlik sorunlarını herkese açık issue yerine [SECURITY.md](SECURITY.md)
üzerinden bildirin.

Taskbar Widgets [MIT Lisansı](LICENSE) ile yayınlanır.

---

<p align="center">
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest/download/TaskbarWidgetsSetup-x64.exe"><strong>Taskbar Widgets'ı indir</strong></a>
  ·
  <a href="https://github.com/pfcdev/TaskbarWidgets/releases/latest">Sürüm notları</a>
  ·
  <a href="https://github.com/pfcdev/TaskbarWidgets/issues">Sorun bildir</a>
</p>
