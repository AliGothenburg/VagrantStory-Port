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
| **PS1 SPU ADPCM & WinMM Ses** | `include/vsport/psx_spu.hpp`, `src/psx_spu.cpp`, `src/win32_audio.cpp` | 512 KB SPU Ses RAM'i, `DMA4` dalga bankası aktarımı, 24 donanım sesi, Sony 4-bit ADPCM IIR + ADSR zarf sentezleyici ve 44.1 kHz Stereo `waveOut` / `.wav` çıkışı |
| **Hibrit AOT Host Runtime** | `include/vsport/runtime.hpp`, `src/runtime.cpp` | 2 MB / 8 MB RAM, 1 KB Scratchpad, BIOS `A0`/`B0`/`C0` + Root Counter HLE, tam-çift yönlü `SIO0` kontrolcü ve sayfa-epoch önbellekli **Overlay-Duyarlı Native x64 AOT + Yorumlayıcı hibrit dağıtıcı** |

## Derleme

```powershell
./build.ps1
```

## Disk İmajı

Araçlar kendi Vagrant Story disk imajınızı (`.cue` + `.bin`) kullanır. Aşağıdaki komutlar disk yolunu
`$Disc` değişkeninden okur; önce PowerShell oturumunda bunu kendi yolunuza göre ayarlayın:

```powershell
$Disc = "C:\Users\anari\Downloads\Minerva_Myrient\Redump\Sony - PlayStation\Vagrant Story (Europe)\Vagrant Story (Europe).cue"
```

> **Not:** `generated_recomp/` diskten üretilen koddur ve `.gitignore` ile depo dışında tutulur
> (oyunun kodunun birebir çevirisi olduğu için paylaşılmamalıdır). Yeni bir klonda önce `vs_recomp`
> ile üretin, ardından `./build.ps1` ile derleyin; klasör yoksa `vs_host` AOT modülleri olmadan
> yalnızca yorumlayıcıyla derlenir.

## Araçların Kullanımı

1. **Disk İmajını İnceleme ve Overlay Analizi (`vs_inspect`):**
   ```powershell
   ./build/vs_inspect.exe --disc $Disc --extract-dir extracted
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
   ./build/vs_host.exe --disc $Disc

   # Headless performans, 24-bit aktif ekran / VRAM ve 44.1 kHz WAV ses çıktısı testi:
   ./build/vs_host.exe --disc $Disc --headless --cycles 1150000000 --dump-display fmv.ppm --dump-wav spu.wav
   ```
   *Pencere Kısayolları:* `F2` Tam 1024×512 VRAM | `F3` 16:9 Geniş Ekran GTE | `F4` HLSL Shader Kipi (Keskin / Bilinear / Trinitron CRT) | `M` Ses Aç/Kapat | `F12` PPM Kaydet | `ESC` Çıkış
