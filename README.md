# Vagrant Story - Native Port & Static Recompilation Engine (`vsport`)

Sıfırdan C++20 ile geliştirilen modüler **Vagrant Story (PS1 -> Windows x64)** yerel port ve statik rekompilasyon altyapısı.

## Mimari Bileşenler

| Modül | Dosyalar | Görev |
|---|---|---|
| **CD-ROM XA / ISO9660** | `include/vsport/cdrom_iso.hpp`, `src/cdrom_iso.cpp` | `.cue` / `.bin` (2352 bayt Mode 2 XA) ve `.iso` (2048 bayt) okuyucu, `SYSTEM.CNF` ayrıştırıcı ve doğrudan LBA sektör erişimi |
| **PS1 CD-ROM XA Kontrolcüsü** | `include/vsport/psx_cdrom.hpp`, `src/psx_cdrom.cpp` | `0x1F801800..0x1F801803` indeksli donanım yazmaçları, `CdlSetloc`/`CdlReadN`/`CdlPause`, `INT1..INT3` IRQ2 durum makinesi ve DMA3 akışı |
| **PS-X EXE & `.PRG` Analizörü** | `include/vsport/psx_exe.hpp`, `src/psx_exe.cpp` | `SLES_027.54` / `SLUS_010.40` çekirdek yükleyici ve diskteki tüm `.PRG` overlay modülleri (`BATTLE`, `INITBTL`, `TITLE`, `ENDING`, `SCREFF2`, `MAINMENU`, `MENU0..MENUF`) için otomatik `JAL->Prologue` KSEG0 taban adresi çözücüsü |
| **MIPS R3000A + COP2 Çözücü** | `include/vsport/mips_decoder.hpp`, `src/mips_decoder.cpp` | Tam MIPS I + COP0 + GTE (`COP2`) talimat çözücü ve disassembler |
| **CFG & C++20 Statik Rekompilatör** | `include/vsport/cfg_lifter.hpp`, `src/cfg_lifter.cpp` | `.data` filtrelemeli fonksiyon keşfi, gecikme yuvası (branch delay slot) korumalı C++20 kod üretimi ve **21 modül / 3.138 yerel x64 fonksiyon** için 64-bit FNV-1a overlay imza kaydı |
| **GTE (Geometry Transform Engine)** | `include/vsport/gte.hpp`, `src/gte.cpp` | Sabit noktalı 3B perspektif dönüşüm (`RTPS`/`RTPT`), yüzey kırpma (`NCLIP`), derinlik sıralama (`AVSZ3`/`AVSZ4`), ışıklandırma ve **16:9 geniş ekran projeksiyon ölçekleme** |
| **PS1 MDEC Video Çözücü** | `include/vsport/psx_mdec.hpp`, `src/psx_mdec.cpp` | `0x1F801820` / `0x1F801824` donanım MDEC, RLE ters niceleme (IQ), 2B ayrılabilir IDCT, BT.601 YCrCb→RGB dönüşümü ve `DMA0`/`DMA1` şeritli **24-bit `RGB888` `.STR` tam hareketli video (FMV)** çözücüsü |
| **PS1 GPU & Yazılım Rasterizer** | `include/vsport/psx_gpu.hpp`, `src/psx_gpu.cpp` | 1024×512 RGB555 VRAM, `GP0`/`GP1` (`Y1..Y2` dikey aralık + 24-bit `RGB888` ekran kipi) paket işlemcisi, `ABR 0..3` yarı-saydamlık, CLUT dokulu üçgen/dörtgen/sprite çizici, DMA6 OTC ve DMA2 bağlı liste (Ordering Table) hattı |
| **Direct3D 11 Donanım Çizici** | `include/vsport/d3d11_renderer.hpp`, `src/d3d11_renderer.cpp` | `R16_UINT` doğrudan GPU VRAM aktarımı, HLSL RGB555/RGB888 çözücü ve 3 ölçekleme kipi (*Keskin Piksel*, *Bilinear*, *Trinitron CRT*) |
| **PS1 SPU ADPCM & WASAPI Ses** | `include/vsport/psx_spu.hpp`, `src/psx_spu.cpp`, `src/win32_audio.cpp` | 512 KB SPU Ses RAM'i, `DMA4` dalga bankası aktarımı, 24 donanım sesi, Sony 4-bit ADPCM IIR + ADSR zarf sentezleyici ve 44.1 kHz Stereo WASAPI (yaklaşık 40–50 ms kuyruk, gerekirse WinMM `waveOut`) / `.wav` çıkışı |
| **Hibrit AOT Host Runtime** | `include/vsport/runtime.hpp`, `src/runtime.cpp` | 2 MB / 8 MB RAM, 1 KB Scratchpad, BIOS `A0`/`B0`/`C0` + Root Counter HLE, tam-çift yönlü `SIO0` kontrolcü ve sayfa-epoch önbellekli **Overlay-Duyarlı Native x64 AOT + Yorumlayıcı hibrit dağıtıcı** |

## Oynamak

`./build.ps1` derlemenin sonunda oyunu `dist/VagrantStory.exe` olarak kopyalar. Bu dosyaya çift
tıklamak yeterlidir (konsol penceresi açılmaz):

- Açılışta **ayarlar penceresi** gelir. Soldaki düğmelerle sayfalar arasında geçilir (Oyun, Grafik,
  Ekran, Ses, Kontroller):
  - **Oyun:** oyun klasörünü seçin (içindeki `.cue`/`.bin`/`.iso` ve `.sbi` otomatik bulunur) ya da
    dosyaları tek tek seçin. Pencere diski doğrular (sürüm, Avrupa sürümünde `.sbi` durumu).
  - **Grafik:** iç çözünürlük (1× orijinal 320×240, 2×–6×; 1080p ekranlar için 5×), çizim,
    ekran kartı, kenar ışığı, köşe hassasiyeti, 3D doku yumuşatma.
  - **Ekran:** pencere / tam ekran, **görüntü oranı** (4:3, 10:7, 3:2, 16:10, 16:9 ya da
    pencereyi doldur), pencere boyutu (480p, 720p, 960p, 1080p, 1200p, 1440p, 4K; genişlik orana
    göre ayarlanır), filtre (keskin piksel, yumuşak, CRT tarama çizgileri), tam sayı ölçekleme.
  - **Çizim:** yüksek çözünürlüklü görüntüyü kimin çizeceği. **Ekran kartı** (varsayılan,
    Direct3D 11) çok daha hızlıdır; **İşlemci** eski yazılım çizicisidir. İkisinin çıktısı
    neredeyse piksel piksel aynıdır. Ekran kartı başlatılamazsa oyun kendiliğinden işlemciye geçer.
  - **Ekran kartı:** birden fazla ekran kartı olan bilgisayarlarda (ör. laptoplarda tümleşik +
    NVIDIA) hangisinin kullanılacağı. **Otomatik** (varsayılan) en güçlüsünü seçer; seçilen kart
    artık takılı değilse yine otomatik seçime dönülür.
  - **Karakter kenar ışığı:** oyun ara sahnelerde ve menüde karakterleri önce parlak beyaz,
    ışığa doğru ~1 piksel kaydırılmış olarak, sonra normal çizer (orijinal kenar ışığı efekti).
    Yüksek iç çözünürlükte bu kayma sert, kalın bir bant olur. **Yumuşak** (varsayılan) ışığı
    yarı saydam çizip dışa doğru söndürür; **Keskin** ince, parlak bir çizgiye indirir;
    **Kapalı** çizmez.
  - **Köşe hassasiyeti:** PS1'in geometri birimi (GTE) her noktayı tam piksele yuvarlar; bu
    yüzden modeller ve kenarlar hareket ederken titrer. **Titreme düzeltmesi** (iç çözünürlük
    2× ve üstü) GTE'nin hesapladığı tam konumu saklar ve yüksek çözünürlüklü çizimde kullanır.
    **Titreme + perspektifli doku** (varsayılan) ayrıca köşelerin derinliğini kullanarak dokuları
    perspektife uygun çizer: zemin ve duvarlardaki dokular artık köşegenler boyunca bükülüp
    kaymaz (PS1 dokuyu ekranda doğrusal yayar). Derinliği bilinmeyen poligonlar PS1'deki gibi
    çizilir. Oyun bu noktaları kendi koduyla kopyaladığı için eşleştirme piksel
    değeriyle yapılır (köşelerin ~%90'ı); aynı piksele düşen birden fazla nokta varsa o köşe
    orijinal konumunda kalır. Oyunun kendisi ve orijinal (1×) görüntü değişmez.
  - **3D sahneyi orana göre genişlet:** açıksa geniş oranlarda 3D sahne esnetilmez, yanlara
    doğru daha fazlası görünür (denenen sahnelerde kenarlarda boşluk görülmedi; 2D yazılar ve
    göstergeler ise yatayda esner). Kapalıysa görüntü seçilen orana esnetilir.
  - Pencere kipinde pencere seçilen oranı korur (kenarından sürükleyince ve F3 ile oran
    değiştirince), bu yüzden etrafta siyah bant kalmaz. Tam ekranda ekranınızın oranını seçin
    (ör. 16:9 monitörde 16:9) ya da "doldur"u kullanın. Tam sayı ölçekleme yalnızca 1× iç
    çözünürlükte uygulanır.
  - **Ses:** sesi kapatma, ses seviyesi, **yankı** (PS1'in reverb birimi; oyun mekân ve zindan
    yankısı için yoğun kullanır, varsayılan açık) ve **kübik ara değerleme** (örnekler arasında
    doğrusal yerine kübik geçiş; tizler biraz daha temiz, varsayılan kapalı, gerçek PS1 doğrusala
    yakındır). Komut satırı: `--reverb 0/1`, `--cubic`.
  - **Titreşim:** oyun DualShock titreşimini kullanır; Xbox / XInput uyumlu kontrolcülerde çalışır
    (Kontroller sayfasından kapatılabilir; DirectInput kontrolcülerde henüz yok).
  - **Anlık kayıt:** oyunun kayıt noktalarından bağımsız olarak **F5** kaydeder, **F7** yükler, **F6** kayıt
    yuvasını değiştirir (1–5; dosyalar hafıza kartının yanında, `VagrantStory_durumN.vss`). Hafıza kartı
    anlık kayda dahil değildir: bir durumu yüklemek kartta yapılmış kayıtları geri almaz.
    Gelişmiş sayfasında iki seçenek daha var: **çıkarken durumu kaydedip açılışta kaldığın yerden devam**
    ve **geri sarma** (F8 basılı tutulunca son ~30 saniye; ~110 MB bellek).
  - **Hile kodları:** Gelişmiş sayfasından açılır. Kodlar exe'nin yanındaki `VagrantStory_hileler.txt`
    dosyasına GameShark / Action Replay biçiminde yazılır (`[Hile adı]` satırı, altında `80012345 03E7`
    gibi kodlar; `[Ad] off` kapalı hile). Desteklenen türler: 80, 30, D0–D3, E0–E3. Kodlar oyunun
    sürümüne özeldir (Avrupa sürümü: SLES-02754). "Hile dosyasını aç…" dosyayı açıklamasıyla oluşturur.
  - **Ara kare üretimi (Grafik sayfası, deneysel):** oyun 2 vblank'te bir yeni kare gösterir
    (25 fps). Açıkken her yeni karenin öncesinde, her poligonu önceki ve yeni karedeki yerinin tam
    ortasında çizen bir ara kare gösterilir; görüntü 50 fps akar. Oyunun kendisi değişmez (oyun
    mantığı 25 fps çalışmaya devam eder). Bunun bedeli, yeni karenin 1 vblank (20 ms) geç
    görünmesidir. Sahne değişimlerinde, oyunun yavaşladığı anlarda, videolarda ve iki kare
    arasında bulunamayan ya da çok uzağa sıçrayan poligonlarda ara kare yapılmaz; o poligonlar
    yeni karedeki yerinde çizilir. Yazılar ve göstergeler (sprite'lar) yerinde kalır. Yalnızca
    ekran kartı çiziminde ve 2× ve üstü iç çözünürlükte çalışır. Komut satırı: `--interp 0/1`.
  - **Doku paketleri:** exe'nin yanındaki `dokular` klasörüne konan PNG'ler oyunun dokularının yerine
    çizilir (2× ve üstü iç çözünürlükte, iki çizim yolunda da; oyunun kendisi ve 1× görüntü değişmez).
    Paket hazırlamak için Gelişmiş sayfasında "Oyunun dokularını dök" açılır: oyun bir dokuyu her
    çizdiğinde, daha önce dökülmemişse `dokular\_dokum` klasörüne
    `<yükleme özeti>-<palet özeti>-<renk derinliği>.png` adıyla yazılır. Görüntü, oyunun VRAM'e bir
    seferde yüklediği alanın tamamıdır, o paletle çizilmiş haliyle; saydam pikseller alfa 0'dır.
    Büyütülmüş ya da yeniden çizilmiş hali **aynı adla** `dokular` klasörüne (alt klasörler olur; `_`
    ile başlayan klasörler okunmaz) konunca kullanılır. Boyut serbesttir (genelde 2×–8×). Çizilirken
    bilinear süzülür; alfası %50'nin altındaki pikseller saydamdır. Yarı saydamlık oyunun kendi doku
    pikselinden gelir. Ad, dosya adının başındaki bu üç parçadır; arkasına `-kılıç` gibi bir not
    eklenebilir. Bilinen sınırlar: oyun dokunun paletini değiştirdiğinde (renk geçişleri) o anda
    paketteki doku kullanılmaz. Doku penceresi (E2) kullanan çizimler ve birden fazla yüklemeye
    yayılan dokular da değiştirilemez. Paket her açılışta yeniden okunur. Komut satırı:
    `--texpack KLASÖR`, `--texdump KLASÖR`.
  - **Gecikme azaltma (runahead, Gelişmiş sayfası):** her vblank'te gerçek kare çalıştırılır (tuşlar,
    ses) ve durumu saklanır. Ardından 1–3 kare ileri çalıştırılıp sonuncusu gösterilir, sonra
    saklanan duruma dönülür. Tuşa basışın sonucu seçilen kare sayısı kadar (20–60 ms) erken görünür.
    Gösterilen kare, normal çalışmanın o kadar sonraki karesiyle piksel piksel aynıdır. Emülasyon
    kare başına 2–4 kat çalışır. Ryzen 9 8945HS + RTX 4060' ta 4× oynanışta saniyede 1 karede 72,
    2 karede 50 gerçek kare (gereken 50) ölçüldü: 1 kare önerilir, 2–3 kare ağır sahnelerde oyunu
    yavaşlatabilir. Açıkken ara kare üretimi kapanır.
    F5/F7, geri sarma ve çıkışta kayıt gerçek zaman çizgisinde çalışır. Komut satırı: `--runahead N`.
  - **RetroAchievements (Başarımlar sayfası):** retroachievements.org hesabınızla giriş yapın; şifre
    saklanmaz, sunucunun verdiği oturum anahtarı ini'ye yazılır. Vagrant Story'nin başarım seti
    (200 başarım) yalnızca **ABD sürümünü (SLUS-01040)** tanır; Avrupa diskinde "başarım seti yok"
    bildirilir. Yalnızca softcore: yeni bir emülatöre hardcore onayı verilmez; anlık kayıt, geri
    sarma ve hileler kullanılabilir. Açılan başarımlar pencere başlığında ve `VagrantStory.log`'da
    görünür. Gecikme azaltmada yalnızca gerçek kareler denetlenir. Resmî
    [rcheevos](https://github.com/RetroAchievements/rcheevos) kütüphanesi (v12.5.0, MIT)
    `third_party/rcheevos` altındadır. `--ra-hash DISK` bir diskin RA özetini yazar.
  - **ABD sürümü:** SLUS-01040 diski de çalışır (60 Hz, `.sbi` gerekmez). Derlenmiş oyun kodu Avrupa
    sürümünden üretildiği için ABD sürümü yorumlayıcıyla çalışır: 4× ekran kartı çiziminde
    saniyede 84–103 kare (gereken 60). Gecikme azaltma bu sürümde gerçek zamana yetişmez.
  - **PAL 60 Hz (Ekran sayfası):** Avrupa sürümü 50 Hz'de saniyede 25 kare çalışır ve bu yüzden ABD
    sürümünden (60 Hz, saniyede 30 kare) ~%17 yavaştır; bu orijinal davranıştır. Açıkken PAL disk 60 Hz
    hızında çalıştırılır ve oyun ABD sürümü hızında akar. Müzik ve yazı hızı zamanlayıcıyla çalıştığı
    için değişmez (ölçüldü). Komut satırı: `--pal60 0/1`.
  - **Bildirimler:** anlık kayıt, geri sarma ve başarım bildirimleri pencere başlığının yanı sıra
    görüntünün alt ortasında yarı saydam bir kutuda gösterilir; tam ekranda da görünür.
  - **Videoları geçme:** oyun açılış videosunu START ile kendisi geçirir; diğer sahnelerde **Tab** hızlı ileri sarar.
  - **Kenar yumuşatma (süper örnekleme):** iç çözünürlük pencereden büyükse görüntü, her ekran pikseli
    kapladığı piksellerin ortalaması alınarak küçültülür; poligon kenarlarındaki tırtıklar yumuşar.
    Bunun için iç çözünürlük 7× ve 8× seçenekleri eklendi (1080p ekranda önerilir; yalnızca ekran
    kartı çiziminde, işlemcide 6× ile sınırlanır).
  - **3D doku filtresi:** Kapalı, Yumuşak (bilinear) ya da **Keskin yumuşatma** (doku pikselleri keskin
    kalır, yalnızca aralarındaki sınır yumuşatılır).
  - **Gelişmiş › İşlemci hızı:** emüle edilen PS1 işlemcisini %150–%300 hızlandırır; oyunun
    kalabalık sahnelerdeki kendi yavaşlamaları azalır (oyun hızlanmaz). Yüklemeler de kısalır.
  - **Kontroller:** her PS1 tuşunun (×, ○, □, △, L1/R1, L2/R2, START, SELECT, L3/R3)
    kontrolcüde hangi tuşa basılınca çalışacağını seçin; "Varsayılana döndür" konuma göre
    eşlemeye geri döner. Yön tuşları ve sol çubuk sabittir; klavye tuşları değişmez.
- Ayarlar exe'nin yanındaki `VagrantStory.ini` dosyasına kaydedilir. "Bu pencereyi her açılışta
  göster" kapatılırsa oyun doğrudan açılır; ayarlara dönmek için oyunu **Shift** tuşuna basılı
  tutarak başlatın.
- Oyunun açılış ve kapanış adımları exe'nin yanındaki `VagrantStory.log` dosyasına yazılır (bir
  sorun olursa nerede durduğunu gösterir). Kapanış 8 saniyede bitmezse oyun kendini sonlandırır;
  arka planda penceresiz bir süreç kalmaz.
- Oyun içinde **F11** / **Alt+Enter** tam ekran, **F4** filtre, **F3** görüntü oranı (sırayla),
  **Shift+F3** 3D genişletme, **M** ses; bu değişiklikler de kaydedilir. **Esc** çıkış (onay sorulur).
- **İç çözünürlük** 3D sahneyi 2×–4× çözünürlükte çizer: polygon kenarları ve doku eşlemesi
  keskinleşir. Oyunun kendi VRAM'i orijinal çözünürlükte kalır (emülasyon doğruluğu korunur),
  yüksek çözünürlüklü kopya yalnızca ekranda gösterilir. Bu kopya 24 bit renkle (true color)
  çizilir: karanlık sahnelerde PS1'in 15 bit renklerinin oluşturduğu sert renk bantları
  görünmez. **3D doku yumuşatma** düşük
  çözünürlüklü dokulardaki blokları giderir (2D yazıları da biraz yumuşatır).
- **Hız:** Yüksek çözünürlüklü görüntüyü varsayılan olarak ekran kartı çizer (Direct3D 11;
  PS1 dokuları, yarı saydamlık, maske biti ve perspektif shader'da PS1 ile aynı tam sayı
  hesabıyla yapılır). İki ekran kartlı laptoplarda güçlü olan (ör. NVIDIA) kendiliğinden seçilir.
  Bu bilgisayarda (RTX 4060 Laptop) açılış sinematiğinin en ağır yerinde 5× ≈ gerçek zamanın
  2,8 katı, 6× ≈ 2,6 katı; işlemcinin tümleşik grafiğiyle (Radeon 780M) 5× ≈ 1,5 katı. **İşlemci** seçilirse çizim
  işlemcinin tüm çekirdeklerine şeritler halinde dağıtılır; o zaman ağır sahnelerde 4× ≈ 1,06
  katı, 5× ≈ 0,8 katı (yavaşlama) olur. `vs_host --perf` her 250 karede hızı yazar
  (`--gpu-hr` / `--cpu-hr` çiziciyi seçer; başsız çalıştırmada varsayılan işlemcidir).
- En net görüntü için iç çözünürlükle birlikte **Keskin piksel** filtresini kullanın; Yumuşak ve
  CRT filtreleri görüntüyü bilerek bulanıklaştırır.
- **Hafıza kartı:** Oyunda kayıt yapılabilir. Kart 1, exe'nin yanındaki
  `saves\VagrantStory_kart1.mcr` dosyasıdır (ayarlar penceresinden değiştirilebilir). Bu, diğer
  emülatörlerin (DuckStation, ePSXe, Mednafen…) okuduğu standart 128 KB `.mcr` biçimidir; dosya
  yoksa boş, formatlanmış bir kart oluşturulur. Değişiklikler oyun yazmayı bitirdikten yaklaşık
  yarım saniye sonra ve çıkışta diske kaydedilir.

Terminalden başlatılırsa `VagrantStory.exe` günlüğü o terminale yazar. Geliştirme ve hata ayıklama
seçenekleri için konsol sürümü `build/vs_host.exe` kullanılır.

## Linux, Steam Deck ve Android

Linux ve Android sürümü aynı oyun çekirdeğini SDL3 ile çalıştırır (`tools/sdl_host.cpp`): pencere,
ses, oyun kolu, dokunmatik tuşlar ve oyun içi menü. Görüntüyü işlemci çizer (Direct3D yok); ekrana
SDL'nin OpenGL / Vulkan / GLES çizicisiyle verilir. Emülasyon Windows ile bire bir aynıdır (aynı kayıt
durumundan aynı kareler, bayt bayt).

- **Menü:** Esc, oyun kolunda **L3 + R3** (iki çubuğa birlikte basmak) ya da Guide tuşu, Android'de
  geri tuşu veya ekranın üstündeki ≡ tuşu. Menüden: durum kaydet / yükle / yuva, ekran oranı,
  3D genişletme, keskin / yumuşak görüntü, iç çözünürlük (1–4×), PAL 60 Hz, ses, dokunmatik tuşlar,
  tam ekran, disk değiştirme, çıkış. Oyun menü açıkken durur.
- **İlk açılış:** Disk ayarlı değilse disk seçme ekranı açılır. Linux / Steam Deck'te pencerenin
  içinde bir dosya gezgini vardır (Steam'in Oyun Modunda da çalışır); Android'de sistemin dosya
  seçicisi açılır.
- **Klavye:** Windows sürümüyle aynı (oklar, Z X A S, Q W E R, Enter, Backspace; F5 / F7 / F6 durum,
  F8 basılı geri sarma, F3 oran, Shift+F3 3D genişletme, F4 görüntü, M ses, F11 tam ekran, Tab hızlı).
- **Ayarlar:** Aynı `VagrantStory.ini` biçimi. Linux'ta `~/.local/share/VagrantStory/` klasöründedir
  (programın yanında `VagrantStory.ini` varsa taşınabilir kullanım için o klasör kullanılır).
  Kayıtlar, hile dosyası ve doku paketleri (`dokular`) de bu klasöre konur.
- **RetroAchievements (Linux):** Sistemdeki `libcurl` ile çalışır. Giriş henüz yalnızca Windows'taki
  ayarlar penceresinden yapılabilir: oradaki `ra_user` / `ra_token` satırlarını Linux'taki ini'ye
  kopyalamak yeterli. Android'de başarımlar henüz yok.

### Steam Deck

1. Masaüstü Modunda `VagrantStory-Port-…-linux-x64.tar.gz` dosyasını bir klasöre açın.
2. Steam → Oyun Ekle → Steam Dışı Oyun Ekle ile `VagrantStory` dosyasını ekleyin.
3. Oyun Modunda başlatın. İlk açılışta disk imajını içerideki dosya gezgininden seçin
   (`.cue` / `.bin` / `.iso`; PAL için aynı adlı `.sbi` aynı klasörde olmalı).

Deck'te varsayılan tam ekrandır; menü **L3 + R3**. Deck'in kontrolleri Steam Input ile oyun kolu
olarak gelir (A = ✕, B = ○, X = □, Y = △).

### Android

APK'yı yükleyip açın, **Disk imajı seç** ile `.bin` (ve PAL için `.sbi`) dosyasını seçin; birden
fazla dosya birlikte seçilebilir. Dosyalar uygulamanın klasörüne bir kez kopyalanır
(`Android/data/io.github.aligothenburg.vagrantstory/files/`; ayarlar ve kayıtlar da orada).
`.cue` gerekmez. Ekrana dokununca dokunmatik tuşlar görünür, oyun kolu kullanılınca gizlenir.
Telefonda varsayılan iç çözünürlük 1×'tir (görüntüyü işlemci çizer); güçlü cihazlarda menüden
artırılabilir.

**Ana ekran simgesi:** İlk açılışta oyun, açılış videosunun sonundaki Rood Inverse'i arka planda
kendi diskinizden yakalar (yarım dakika kadar sürer, oyunu yavaşlatmaz) ve telefon "Ana ekrana
eklensin mi?" diye sorar. Onaylarsanız ana ekranda bu resimle oyunu açan bir kısayol çıkar. Resim
`kapak.png` olarak uygulama klasörüne kaydedilir; kısayol menüdeki **Ana ekran simgesi** ile
yeniden istenebilir.

## Derleme

```powershell
./build.ps1
```

**Linux** (SDL3 kaynağını CMake kendisi indirir; `-DVSPORT_SDL_SOURCE_DIR=...` ile yerel bir
klasör de verilebilir):

```bash
sudo apt install build-essential cmake ninja-build libasound2-dev libpulse-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libegl1-mesa-dev libdbus-1-dev libudev-dev libpipewire-0.3-dev libdecor-0-dev
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DVSPORT_LINK_AOT=OFF
cmake --build build/linux
```

`build/linux/VagrantStory` oyunun kendisidir (SDL3 ve C++ çalışma kütüphanesi içine bağlıdır;
yalnızca `glibc` 2.38+ gerekir), `vs_host` başsız test aracıdır.

**Android** (JDK 17 ve Android SDK + NDK 29 gerekir):

```powershell
cd android
./gradlew assembleRelease
```

APK: `android/app/build/outputs/apk/release/app-release.apk` (arm64 ve x86_64).

## Disk İmajı

Araçlar kendi Vagrant Story disk imajınızı (`.cue` + `.bin`) kullanır. Aşağıdaki komutlar disk yolunu
`$Disc` değişkeninden okur; önce PowerShell oturumunda bunu kendi yolunuza göre ayarlayın:

```powershell
$Disc = "C:\Users\anari\Downloads\Minerva_Myrient\Redump\Sony - PlayStation\Vagrant Story (Europe)\Vagrant Story (Europe).cue"
```

### Alt kanal verisi (.sbi) — PAL sürümü için gerekli

Avrupa sürümü (SLES-02754) **LibCrypt** korumalıdır: New Game'de birkaç sektörün alt kanal (Q)
verisini kontrol eder. `.bin` imajı bu veriyi içermez; diskin Redump `.sbi` dosyası olmadan oyun
New Game'den sonra "NOW LOADING"de kalır. `.sbi` dosyasını `.cue` ile aynı klasöre aynı adla
koyarsanız otomatik yüklenir; başka bir yerdeyse `--sbi` ile belirtin:

```powershell
$Sbi = "C:\Users\anari\Downloads\Vagrant Story (Europe)\Vagrant Story (Europe).sbi"
./build/vs_host.exe --disc $Disc --sbi $Sbi
```

ABD sürümünde (SLUS-01040) bu koruma yoktur.

> **Not:** `generated_recomp/` diskten üretilen koddur ve `.gitignore` ile depo dışında tutulur
> (oyunun kodunun birebir çevirisi olduğu için paylaşılmamalıdır). Yeni bir klonda önce `vs_recomp`
> ile üretin, ardından `./build.ps1` ile derleyin; klasör yoksa `vs_host` AOT modülleri olmadan
> yalnızca yorumlayıcıyla derlenir.

## Araçların Kullanımı

1. **Disk İmajını İnceleme ve Overlay Analizi (`vs_inspect`):**
   ```powershell
   ./build/vs_inspect.exe --disc $Disc --extract-dir extracted

   # Çekirdek EXE'yi ya da bir overlay'i yükleme adresinde disassemble etme:
   ./build/vs_inspect.exe --disc $Disc --disasm 0x80010B14 64
   ./build/vs_inspect.exe --disc $Disc --prg BATTLE/BATTLE.PRG --disasm 0x800D51D0 40
   ```

2. **Statik C++20 Kod Üretimi (`vs_recomp`):**
   ```powershell
   ./build/vs_recomp.exe --disc $Disc --out-dir generated_recomp
   ```

3. **Yerel Windows Host Uygulaması (`vs_host`):**
   ```powershell
   # Donanım (GTE + DMA6 OTC + DMA2 GPU + Win32) canlı doğrulama demosu:
   ./build/vs_host.exe --self-demo

   # Disk imajıyla önyükleme (pencere modunda Native C++20 AOT aktif):
   ./build/vs_host.exe --disc $Disc --sbi $Sbi

   # Headless çalıştırma sonunda PS1 RAM'ini (2 MB) dosyaya dökme (hata ayıklama):
   ./build/vs_host.exe --disc $Disc --sbi $Sbi --headless --cycles 900000000 --dump-ram ram.bin

   # Headless modda belirli VBlank karelerinde tuş basma (BUTON@VBLANK[:KARE], virgülle ayrılır):
   ./build/vs_host.exe --disc $Disc --headless --cycles 470000000 --press START@370,START@430,DOWN@650:4 --dump-display menu.ppm

   # Her N karede bir ekran görüntüsü kaydetme (ör. 1x ile 4x'i kare kare karşılaştırmak için);
   # --rim 0/1/2 karakter kenar ışığını seçer:
   ./build/vs_host.exe --disc $Disc --sbi $Sbi --headless --cycles 20000000000 --scale 4 --dump-series kareler 50

   # Headless performans, 24-bit aktif ekran / VRAM ve 44.1 kHz WAV ses çıktısı testi:
   ./build/vs_host.exe --disc $Disc --headless --cycles 1150000000 --dump-display fmv.ppm --dump-wav spu.wav

   # Birim testleri; ikincisi ekran kartı çizicisini işlemciyle karşılaştırır (her tür primitif,
   # yarı saydamlık, maske, doku penceresi, perspektif, kendi alanından dokulanan sprite).
   # Microsoft'un WARP yazılım cihazında çalışır, ekran kartı gerekmez:
   ./build/vsport_tests.exe
   ./build/vsport_gpu_tests.exe
   ```
   Oyun, seçtiği video kipine göre gerçek zamanlı hızda (PAL 50 Hz / NTSC 60 Hz) çalışır.

   **Kontroller:** Klavye ve gamepad birlikte kullanılabilir; kontrolcü oyun açıkken de takılabilir.
   Xbox ve XInput uyumlu kontrolcüler XInput ile, DualShock 4 / DualSense ve diğer genel
   gamepad'ler DirectInput ile okunur. Sol analog çubuk yön tuşu gibi davranır.

   | PS1 | Klavye | Xbox (XInput) | DualShock 4 / DualSense |
   |---|---|---|---|
   | Yön | Ok tuşları | D-pad / sol çubuk | D-pad / sol çubuk |
   | × | `Z` | A | × |
   | ○ | `X` | B | ○ |
   | □ | `A` | X | □ |
   | △ | `S` | Y | △ |
   | L1 / R1 | `Q` / `W` | LB / RB | L1 / R1 |
   | L2 / R2 | `E` / `R` | LT / RT | L2 / R2 |
   | START | `Enter` | Menu (Start) | Options |
   | SELECT | `Backspace` | View (Back) | Share / Create |

   Kontrolcü tuşları ayar penceresindeki **Kontroller** sayfasından değiştirilebilir
   (`VagrantStory.ini` içinde `pad_cross=circle` gibi satırlar olarak saklanır).

   *Pencere Kısayolları:* `F11` / `Alt+Enter` Tam ekran | `Tab` (basılı tut) Hızlı ileri | `F2` Tam 1024×512 VRAM | `F3` Görüntü oranı (4:3 → 10:7 → 3:2 → 16:10 → 16:9 → doldur) | `Shift+F3` 3D genişletme (GTE) | `F4` HLSL Shader Kipi (Keskin / Bilinear / Trinitron CRT) | `M` Ses Aç/Kapat | `F12` PPM Kaydet | `ESC` Çıkış
