# Storing and playing video from internal flash
# Dahili flash'tan video saklama ve yayınlama

*Türkçe açıklama aşağıda — [Türkçe](#türkçe).*

Instead of the built-in test card, ESP32-DATV-SA can transmit a short video clip in a loop, stored in the
ESP32-C3's own 4 MB flash. No PC is needed while it transmits. The video lives in its own area of the
flash (address **`0x110000`**), so you can change the video at any time **without changing the firmware**,
and you can update the firmware without losing the video.

| | |
|---|---|
| Length | up to about **35 seconds**, played in a loop |
| Picture | H.264, 480 pixels wide (default), 15 frames per second, about 370 kb/s |
| Sound | MP2 mono, 32 kb/s |
| Transmission | DVB-S2, QPSK, FEC 3/4, pilots on, roll-off 0.35 |
| Symbol rate | **330.579 kS/s** (slightly below the test card's 333 kS/s, see [why](#why-33058-kss)) |
| Flash address | **`0x110000`** (the `video` partition, 2.9 MB) |

The work has three parts:

1. [Prepare the video](#1-prepare-the-video) on a PC: turn any video file into `video.bin`.
2. Write it to the board with the web flasher at <https://espressif.github.io/esptool-js/>:
   [firmware and video together](#2-firmware-and-video-together-new-board) on a new board, or
   [only the video](#3-only-the-video-change-the-video) to change the video.
3. [Choose it on the board and receive it](#4-choose-the-source-and-receive).

## 1. Prepare the video

**In the browser:** [`docs/converter/`](converter/) is a web page that does the same as `make_video.py` below, with
nothing to install (FFmpeg runs in the browser as WebAssembly; the file is not uploaded). Open it from a web server, e.g.
`python3 -m http.server -d docs 8000` in the repository and then <http://localhost:8000/converter/>: choose the
file, the start and the length, press *Convert* and download `…_video.bin`. Then continue with section 2 or 3.

**On the command line:** this step needs a computer (Windows, macOS or Linux) with **Python 3** and **FFmpeg**. It is done once per
video; writing it to the board later needs only a web browser.

1. Install **Python 3** (<https://www.python.org/downloads/>; on Windows tick *Add python.exe to PATH*) and
   **FFmpeg** (<https://ffmpeg.org/download.html>; Linux: `sudo apt install ffmpeg`, macOS: `brew install ffmpeg`).
   Check in a terminal that `ffmpeg -version` works.
2. Get the project: <https://github.com/barisdinc/ESP32-DATV-SA> → *Code* → *Download ZIP* and unzip it,
   or `git clone https://github.com/barisdinc/ESP32-DATV-SA.git`.
3. Open a terminal in the project folder and install the Python packages once:

   ```sh
   python3 -m pip install -r host/requirements.txt
   ```

   (On Windows use `py` instead of `python3`, here and below.)
4. Make the video file. The example takes 20 seconds of the included Sintel trailer, starting at second 10:

   ```sh
   python3 host/make_video.py media/sintel_trailer.mp4 --start 10 --seconds 20 -o video.bin
   ```

   Any video file FFmpeg can read works (`.mp4`, `.mkv`, `.mov`, `.avi`, …). When it is done it prints the
   length of the loop, the file size and the address to write it to:

   ```
   media/sintel_trailer.mp4 from 10 s: 480 px, 15 fps, video 369 kb/s, audio 32 kb/s; ... 20.1 s loop
   video.bin: 1668260 bytes (54 % of the partition). Write it at 0x110000: ...
   ```

| Option | Meaning |
|---|---|
| `--start S` | where the clip starts in the file, in seconds (default 0) |
| `--seconds S` | length of the loop, in seconds (default 20, at most about 35) |
| `--width W` | picture width in pixels (default 480; smaller = sharper at this bit rate, larger = more detail but more blocky in motion) |
| `--service-name NAME` | service name the receiver shows (default `yATOLYE-TV`) |
| `--service-provider NAME` | provider name (default `Technology Community`) |
| `--ts preview.ts` | also writes the transport stream, to watch it on the PC first (VLC or `ffplay preview.ts`) |
| `-o FILE` | output file (default `video.bin`) |

**Tip:** the receiver shows a short hiccup each time the loop starts again. Choose `--start` and
`--seconds` so that the clip begins and ends at a scene cut, or at a dark moment, and the hiccup is hard to see.

## 2. Firmware and video together (new board)

Use this for a new board, or when you also update the firmware. You need the firmware file
**`ESP32-DATV-SA-<version>.bin`** from the [latest release](https://github.com/barisdinc/ESP32-DATV-SA/releases/latest)
(a version with video support) and the `video.bin` you made. Use **Chrome**, **Edge** or **Opera** on a desktop computer;
Firefox and Safari can not talk to USB devices.

1. Plug the ESP32-C3 into the computer with a USB **data** cable.
2. Open **<https://espressif.github.io/esptool-js/>**.
3. Leave *Baudrate* as it is and click **Connect**. Choose **USB JTAG/serial debug unit**
   (Windows: a *COM* port, macOS: `cu.usbmodem…`, Linux: `ttyACM0`) and click **Connect**.
4. *(New board only)* click **Erase Flash** and wait until it is done.
   **Erase Flash deletes the video too** — never use it when you only want to change the firmware or the video.
5. In the *Program* section make **two rows** (click **Add File** to get a second row):

   | Flash Address | File |
   |---|---|
   | **`0x0`** | `ESP32-DATV-SA-<version>.bin` |
   | **`0x110000`** | `video.bin` |

   The address field of a new row may show `0x1000`: overwrite it. Both addresses must be exactly as above.
6. Click **Program** and wait for 100 % (about one minute).
7. Press the board's **RST** button (or unplug and plug it in again). It now transmits the video.

## 3. Only the video (change the video)

The firmware and the saved front-panel settings stay as they are; only the video is replaced.

1. Steps 1–3 as above (plug in, open esptool-js, **Connect**).
2. **Do not** click *Erase Flash*.
3. In the *Program* section make **one row** only:

   | Flash Address | File |
   |---|---|
   | **`0x110000`** | `video.bin` |

   Remove any other row (the firmware row) with its *Remove* button.
4. Click **Program**, wait for 100 %, press **RST**.

At power-up the firmware checks the video (a header and a CRC32 over the whole video). An incomplete or
damaged write is never transmitted: the board then sends the test card and the screen shows `VIDEO none`.

**Command line** (Python, `pip install esptool`):

```sh
esptool --chip esp32c3 write-flash 0x110000 video.bin               # only the video
esptool --chip esp32c3 write-flash 0x0 ESP32-DATV-SA-<version>.bin 0x110000 video.bin   # both
esptool --chip esp32c3 erase-region 0x110000 0x1000                 # remove the video
```

(older esptool versions: `esptool.py … write_flash`, `erase_region`)

## 4. Choose the source and receive

With the optional OLED / encoder front panel, the fourth line of the screen is the **source**:

```
 BEACON TX       PA OFF
 2300.500           MHz
 VIDEO 20s SR331  -27dB      <- source: VIDEO 20s SR331  or  TEST CARD 3/4
 SR 333 kS/s     pilots
 S yATOLYE-TV
 P TEST PATTERN
```

Turn the encoder to that field, press, turn to choose **`VIDEO …`** or **`TEST CARD 3/4`**, and press again: the
board switches at once and remembers the choice. `VIDEO none` means the flash holds no valid video; the test
card goes out instead. Without a front panel the board plays the video whenever one is present.

Receiver settings for the video:

| | |
|---|---|
| Frequency | as set on the panel (default 2300.500 MHz) |
| Standard | DVB-S2, QPSK, FEC 3/4, pilots on, roll-off 0.35 |
| Symbol rate | **330.579 kS/s** (if the receiver takes only whole numbers: 330 or 331) |

The test card stays at 333 kS/s: change the receiver's symbol rate when you switch the source.
Frequency, level and PA on the panel apply to both; symbol rate, pilots and the names on the panel apply
to the test card only — the video brings its own.

## If something does not work

| Problem | Fix |
|---|---|
| The screen shows `VIDEO none` | The video was not written to **`0x110000`**, the write did not finish, or *Erase Flash* removed it. Write `video.bin` again (section 3). A `video.bin` made with an older `make_video.py` must be made again. |
| No picture, the receiver does not lock | Set the symbol rate to **330.579 kS/s** (not 333). Check that the source on the panel is `VIDEO …`. |
| The test card was lost after flashing the firmware | The firmware must go to **`0x0`** (not `0x1000`). |
| The video was lost after flashing the firmware | *Erase Flash* was used. Write `video.bin` again. |
| `make_video.py`: "do not fit the partition" | Lower `--seconds`. |
| `make_video.py`: FFmpeg not found | Install FFmpeg and check that `ffmpeg -version` works in the same terminal. |

The board also reports the video on its USB serial port at power-up (115200 baud, any terminal):
`VIDEO yATOLYE-TV / Technology Community, 6656400 symbols at 330579 Bd`, or `VIDEO none`, `VIDEO bad CRC`, `VIDEO bad length`.

## Why 330.58 kS/s?

The video (1.7 MB for 20 s) does not fit the ESP32-C3's RAM, so the transmit loop reads it from the flash
while it runs. Reading the flash through the cache stops the processor for about 320 cycles at a time —
far longer than the 20 cycles it has per DAC sample — and the receiver loses lock. Instead the second flash
controller (SPI1) reads 64 bytes at a time in the background into a small RAM ring; every 16 symbols the loop
copies one word out of SPI1's buffer, which takes about 20 cycles. To make room for that, each symbol takes
484 instead of 480 processor cycles: 160 MHz / 484 = 330.579 kS/s. The details are in the
[main README](https://github.com/barisdinc/ESP32-DATV-SA#stand-alone-video).

---

## Türkçe

ESP32-DATV-SA, dahili test yayını yerine ESP32-C3'ün kendi 4 MB flash belleğinde saklanan kısa bir
videoyu döngüde yayınlayabilir. Yayın sırasında bilgisayar gerekmez. Video flash'ta kendine ait bir
bölgede (**`0x110000`** adresi) durur; böylece videoyu **firmware'i değiştirmeden** istediğiniz zaman
değiştirebilir, firmware'i de videoyu kaybetmeden güncelleyebilirsiniz.

| | |
|---|---|
| Süre | en fazla yaklaşık **35 saniye**, döngüde |
| Görüntü | H.264, 480 piksel genişlik (varsayılan), saniyede 15 kare, yaklaşık 370 kb/s |
| Ses | MP2 mono, 32 kb/s |
| Yayın | DVB-S2, QPSK, FEC 3/4, pilotlar açık, roll-off 0.35 |
| Sembol hızı | **330.579 kS/s** (test yayınının 333 kS/s'inden biraz düşük, [neden](#neden-33058-kss)) |
| Flash adresi | **`0x110000`** (`video` bölümü, 2,9 MB) |

İş üç bölümden oluşur:

1. Bilgisayarda [videoyu hazırlamak](#1-videoyu-hazırlamak): herhangi bir video dosyasından `video.bin` üretmek.
2. <https://espressif.github.io/esptool-js/> web yükleyicisiyle karta yazmak: yeni kartta
   [firmware ve video birlikte](#2-firmware-ve-video-birlikte-yeni-kart), videoyu değiştirmek için
   [yalnızca video](#3-yalnızca-video-videoyu-değiştirmek).
3. [Kartta seçip almak](#4-kaynağı-seçmek-ve-yayını-almak).

### 1. Videoyu hazırlamak

**Tarayıcıda:** [`docs/converter/`](converter/) aşağıdaki `make_video.py` ile aynı işi yapan, kurulum gerektirmeyen bir web
sayfasıdır (FFmpeg tarayıcıda WebAssembly olarak çalışır; dosya hiçbir yere yüklenmez). Bir web sunucusundan açın, ör.
depoda `python3 -m http.server -d docs 8000` ve ardından <http://localhost:8000/converter/>: dosyayı, başlangıcı ve
uzunluğu seçin, *Dönüştür*'e basın ve `…_video.bin`'i indirin. Sonra 2. ya da 3. bölümle devam edin.

**Komut satırında:** bu adım için **Python 3** ve **FFmpeg** kurulu bir bilgisayar (Windows, macOS veya Linux) gerekir. Her video
için bir kez yapılır; karta yazmak için daha sonra yalnızca web tarayıcısı yeterlidir.

1. **Python 3**'ü (<https://www.python.org/downloads/>; Windows'ta *Add python.exe to PATH* kutusunu işaretleyin) ve
   **FFmpeg**'i (<https://ffmpeg.org/download.html>; Linux: `sudo apt install ffmpeg`, macOS: `brew install ffmpeg`) kurun.
   Terminalde `ffmpeg -version` komutunun çalıştığını kontrol edin.
2. Projeyi indirin: <https://github.com/barisdinc/ESP32-DATV-SA> → *Code* → *Download ZIP* ve ZIP'i açın,
   ya da `git clone https://github.com/barisdinc/ESP32-DATV-SA.git`.
3. Proje klasöründe bir terminal açın ve Python paketlerini bir kez kurun:

   ```sh
   python3 -m pip install -r host/requirements.txt
   ```

   (Windows'ta burada ve aşağıda `python3` yerine `py` yazın.)
4. Video dosyasını üretin. Örnek, projedeki Sintel fragmanının 10. saniyesinden başlayan 20 saniyeyi alır:

   ```sh
   python3 host/make_video.py media/sintel_trailer.mp4 --start 10 --seconds 20 -o video.bin
   ```

   FFmpeg'in okuyabildiği her video dosyası olur (`.mp4`, `.mkv`, `.mov`, `.avi`, …). İş bitince döngünün
   süresini, dosya boyutunu ve yazılacağı adresi yazar:

   ```
   media/sintel_trailer.mp4 from 10 s: 480 px, 15 fps, video 369 kb/s, audio 32 kb/s; ... 20.1 s loop
   video.bin: 1668260 bytes (54 % of the partition). Write it at 0x110000: ...
   ```

| Seçenek | Anlamı |
|---|---|
| `--start S` | dosyada klibin başladığı yer, saniye (varsayılan 0) |
| `--seconds S` | döngünün süresi, saniye (varsayılan 20, en fazla yaklaşık 35) |
| `--width W` | görüntü genişliği, piksel (varsayılan 480; küçük = bu bit hızında daha net, büyük = daha ayrıntılı ama harekette daha bloklu) |
| `--service-name AD` | alıcının gösterdiği servis adı (varsayılan `yATOLYE-TV`) |
| `--service-provider AD` | sağlayıcı adı (varsayılan `Technology Community`) |
| `--ts onizleme.ts` | taşıma akışını ayrıca yazar; önce bilgisayarda izlemek için (VLC veya `ffplay onizleme.ts`) |
| `-o DOSYA` | çıktı dosyası (varsayılan `video.bin`) |

**İpucu:** alıcıda döngü her başa sardığında kısa bir takılma olur. `--start` ve `--seconds`'ı klip bir sahne
geçişinde ya da karanlık bir anda başlayıp bitecek şekilde seçerseniz takılma neredeyse fark edilmez.

### 2. Firmware ve video birlikte (yeni kart)

Yeni bir kart için ya da firmware'i de güncellerken kullanın. [Son sürüm](https://github.com/barisdinc/ESP32-DATV-SA/releases/latest)
sayfasındaki **`ESP32-DATV-SA-<sürüm>.bin`** firmware dosyası (video destekli bir sürüm) ve hazırladığınız `video.bin` gerekir.
Masaüstünde **Chrome**, **Edge** veya **Opera** kullanın; Firefox ve Safari USB cihazlarla konuşamaz.

1. ESP32-C3'ü **veri taşıyan** bir USB kablosuyla bilgisayara takın.
2. **<https://espressif.github.io/esptool-js/>** adresini açın.
3. *Baudrate*'e dokunmadan **Connect**'e tıklayın. **USB JTAG/serial debug unit** portunu seçin
   (Windows: *COM* portu, macOS: `cu.usbmodem…`, Linux: `ttyACM0`) ve **Connect**'e basın.
4. *(Yalnızca yeni kartta)* **Erase Flash**'a tıklayın ve bitmesini bekleyin.
   **Erase Flash videoyu da siler** — yalnızca firmware'i ya da videoyu değiştirirken asla kullanmayın.
5. *Program* bölümünde **iki satır** oluşturun (ikinci satır için **Add File**'a tıklayın):

   | Flash Address | Dosya |
   |---|---|
   | **`0x0`** | `ESP32-DATV-SA-<sürüm>.bin` |
   | **`0x110000`** | `video.bin` |

   Yeni satırın adres alanında `0x1000` yazabilir: üzerine yazın. İki adres de tam olarak yukarıdaki gibi olmalı.
6. **Program**'a tıklayın ve %100 olmasını bekleyin (yaklaşık bir dakika).
7. Kartın **RST** düğmesine basın (ya da çıkarıp tekrar takın). Kart artık videoyu yayınlar.

### 3. Yalnızca video (videoyu değiştirmek)

Firmware ve ön panelde kaydedilmiş ayarlar olduğu gibi kalır; yalnızca video değişir.

1. Yukarıdaki 1–3. adımlar (kartı takın, esptool-js'i açın, **Connect**).
2. *Erase Flash*'a **tıklamayın**.
3. *Program* bölümünde **yalnızca bir satır** olsun:

   | Flash Address | Dosya |
   |---|---|
   | **`0x110000`** | `video.bin` |

   Başka bir satır (firmware satırı) varsa *Remove* düğmesiyle kaldırın.
4. **Program**'a tıklayın, %100'ü bekleyin, **RST**'ye basın.

Firmware açılışta videoyu kontrol eder (bir başlık ve videonun tamamı üzerinden CRC32). Yarım kalmış ya da
bozuk bir yazma asla yayınlanmaz: kart o zaman test yayınını gönderir ve ekranda `VIDEO none` görünür.

**Komut satırı** (Python, `pip install esptool`):

```sh
esptool --chip esp32c3 write-flash 0x110000 video.bin               # yalnızca video
esptool --chip esp32c3 write-flash 0x0 ESP32-DATV-SA-<sürüm>.bin 0x110000 video.bin   # ikisi birden
esptool --chip esp32c3 erase-region 0x110000 0x1000                 # videoyu silmek
```

(eski esptool sürümlerinde: `esptool.py … write_flash`, `erase_region`)

### 4. Kaynağı seçmek ve yayını almak

İsteğe bağlı OLED / enkoder ön panelinde ekranın dördüncü satırı **kaynaktır**:

```
 BEACON TX       PA OFF
 2300.500           MHz
 VIDEO 20s SR331  -27dB      <- kaynak: VIDEO 20s SR331  ya da  TEST CARD 3/4
 SR 333 kS/s     pilots
 S yATOLYE-TV
 P TEST PATTERN
```

Enkoderle bu alana gelin, basın, çevirerek **`VIDEO …`** ya da **`TEST CARD 3/4`**'ü seçin ve tekrar basın: kart
hemen geçiş yapar ve seçimi hatırlar. `VIDEO none`, flash'ta geçerli bir video olmadığı anlamına gelir; onun yerine
test yayını gider. Ön panel yoksa kart, video varsa her zaman videoyu yayınlar.

Video için alıcı ayarları:

| | |
|---|---|
| Frekans | panelde ayarlı olan (varsayılan 2300.500 MHz) |
| Standart | DVB-S2, QPSK, FEC 3/4, pilotlar açık, roll-off 0.35 |
| Sembol hızı | **330.579 kS/s** (alıcı yalnızca tam sayı alıyorsa: 330 veya 331) |

Test yayını 333 kS/s'te kalır: kaynağı değiştirince alıcının sembol hızını da değiştirin. Paneldeki frekans,
seviye ve PA ikisine de uygulanır; sembol hızı, pilotlar ve isimler yalnızca test yayınına uygulanır — video
kendi ayarlarını getirir.

### Sorun giderme

| Sorun | Çözüm |
|---|---|
| Ekranda `VIDEO none` yazıyor | Video **`0x110000`**'a yazılmamış, yazma tamamlanmamış ya da *Erase Flash* silmiş. `video.bin`'i tekrar yazın (3. bölüm). Eski bir `make_video.py` ile üretilmiş `video.bin` yeniden üretilmeli. |
| Görüntü yok, alıcı kilitlenmiyor | Sembol hızını **330.579 kS/s** yapın (333 değil). Paneldeki kaynağın `VIDEO …` olduğunu kontrol edin. |
| Firmware yükledikten sonra test yayını da yok | Firmware **`0x0`**'a yazılmalı (`0x1000` değil). |
| Firmware yükledikten sonra video kayboldu | *Erase Flash* kullanılmış. `video.bin`'i tekrar yazın. |
| `make_video.py`: "do not fit the partition" | `--seconds`'ı düşürün. |
| `make_video.py`: FFmpeg bulunamadı | FFmpeg'i kurun ve aynı terminalde `ffmpeg -version`'ın çalıştığını kontrol edin. |

Kart açılışta videoyu USB seri portundan da bildirir (115200 baud, herhangi bir terminal):
`VIDEO yATOLYE-TV / Technology Community, 6656400 symbols at 330579 Bd`, ya da `VIDEO none`, `VIDEO bad CRC`, `VIDEO bad length`.

### Neden 330.58 kS/s?

Video (20 saniye için 1,7 MB) ESP32-C3'ün RAM'ine sığmaz; bu yüzden yayın döngüsü onu çalışırken flash'tan okur.
Flash'ı önbellek üzerinden okumak işlemciyi her seferinde yaklaşık 320 çevrim durdurur — DAC örneği başına sahip
olduğu 20 çevrimden çok daha uzun — ve alıcı kilidi kaybeder. Bunun yerine ikinci flash denetleyicisi (SPI1) arka
planda 64'er bayt okuyup küçük bir RAM halkasına koyar; döngü her 16 sembolde SPI1'in tamponundan bir kelime kopyalar,
bu da yaklaşık 20 çevrim sürer. Buna yer açmak için her sembol 480 yerine 484 işlemci çevrimi sürer:
160 MHz / 484 = 330.579 kS/s. Ayrıntılar [ana README](https://github.com/barisdinc/ESP32-DATV-SA#stand-alone-video)'de.
