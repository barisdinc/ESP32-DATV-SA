# Flash ESP32-DATV-SA onto an ESP32-C3 — no software to install
# ESP32-C3'e ESP32-DATV-SA yükleme — hiçbir program kurmadan

*Türkçe açıklama aşağıda — [Türkçe](#türkçe).*

This guide puts the ready-made ESP32-DATV-SA firmware onto a brand-new ESP32-C3 board
(for example an ESP32-C3 SuperMini). You do **not** need ESP-IDF, Python, drivers or any other
program. A web browser does the whole job, on Windows, macOS and Linux.

After flashing, the board transmits a DVB-S2 test card on its own as soon as it gets power:

| | |
|---|---|
| Frequency | 2300.500 MHz |
| Level | low (−27 dB below the usual level) |
| Standard | DVB-S2, QPSK, FEC 3/4, pilots on, roll-off 0.35 |
| Symbol rate | 333 kS/s |
| Service | `yATOLYE-TV` / `Technology Community` |

## What you need

- An **ESP32-C3** board with a USB connector (4 MB flash, as on almost all boards).
- A USB cable that carries **data** (some cheap cables only charge — if nothing shows up, try another cable).
- A computer with **Google Chrome**, **Microsoft Edge** or **Opera** (desktop). Firefox and Safari can
  not talk to USB devices, so they will not work.
- The file **`ESP32-DATV-SA-<version>.bin`** from the
  [latest release](https://github.com/barisdinc/ESP32-DATV-SA/releases/latest)
  (it is also inside the firmware ZIP).

## Steps

1. **Download** `ESP32-DATV-SA-<version>.bin` from the release page.
2. **Plug** the ESP32-C3 into the computer.
3. Open **<https://espressif.github.io/esptool-js/>** in Chrome or Edge
   (this is the official web flasher from Espressif, the maker of the chip).
4. Leave *Baudrate* at its default and click **Connect**.
   A window lists the serial ports: choose **USB JTAG/serial debug unit**
   (Windows: a *COM* port, macOS: `cu.usbmodem…`, Linux: `ttyACM0`) and click **Connect**.
5. *(Recommended for a new board)* Click **Erase Flash** and wait until it reports that it is done.
6. In the *Program* section (if the file table is empty, click **Add File** once):
   - set **Flash Address** to **`0x0`** (the default `0x1000` is wrong for the ESP32-C3),
   - click the file button next to it (*Choose File* / *Browse*) and select the `.bin` you downloaded.
7. Click **Program** and wait for 100 %. It takes about half a minute.
8. Unplug the board and plug it in again (or press its **RST** button). It now transmits.

That is all. Tune your DVB-S2 receiver (MiniTiouner, SDRangel, a satellite set-top box with a
downconverter, …) to the settings in the table above. With the optional OLED / encoder
front panel you can change frequency, level, symbol rate and the names on the device
(see the [main README](https://github.com/barisdinc/ESP32-DATV-SA#front-panel)).

## Your own video (optional)

Instead of the test card the board can loop a video of up to about **35 seconds**, stored in its flash
at **`0x110000`**. The firmware stays the same: to change the video you only write a new video file.

1. Make the file on a PC with Python 3 and FFmpeg (from the repository, after `pip install -r host/requirements.txt`):
   `python3 host/make_video.py my_film.mp4 --start 10 --seconds 20 -o video.bin`
   (`--start` and `--seconds` choose the part to take; a loop that starts and ends at a scene cut hides the short hiccup where it repeats).
2. In esptool-js, flash the firmware as above, then click **Add File** for a second row:
   **Flash Address `0x110000`**, file `video.bin`, and click **Program**. To change only the video later,
   program just that row; the firmware is not touched.
3. Press **RST**. The board plays the video in a loop; the front panel shows `VIDEO 20s SR331`.
   **Set the receiver to 330.579 kS/s** (330 or 331 if it only takes whole numbers): the video uses a symbol rate
   slightly below the test card's 333 kS/s. Frequency, level and the other settings are the same.

**Erase Flash** removes the video too; the board then transmits the test card again.
With esptool you can remove only the video: `esptool --chip esp32c3 erase-region 0x110000 0x1000`.

## If something does not work

| Problem | Fix |
|---|---|
| No port appears after *Connect* | Try another USB cable (a data cable) and another USB port. Then put the board in download mode: hold the **BOOT** button, press and release **RST** (or plug in the cable while holding BOOT), release BOOT, and click *Connect* again. |
| *Failed to connect* / timeout | Same as above: start download mode with BOOT + RST and try again. |
| Linux: port is visible but access is denied | Run `sudo usermod -aG dialout $USER` once, then log out and back in. Chromium installed as a *snap* can not open serial ports; use Chrome or Edge from their own packages. |
| Windows 7/8 has no driver | Use Windows 10 or 11: the ESP32-C3 USB port needs no driver there. |
| Flashed, but no signal | Flash address must be **`0x0`**. Press **RST** after flashing. Check the receiver: 2300.500 MHz, 333 kS/s, DVB-S2. The level is low on purpose: put the antennas close together. |

## Notes

- The beacon starts at a low level (−27 dB); even at full level the ESP32-C3 puts out only a few milliwatts. The **PA enable** output (GPIO3) stays **low**
  in beacon mode unless you switch it on from the front panel.
- Transmitting on 2.4 GHz with a video signal requires an amateur radio licence in most countries.
  Use an attenuator or a dummy load for bench tests.
- The board can still be used with the PC software of the main project: as soon as the PC sends a
  command over USB, the beacon stops and the PC takes over. See the [main README](https://github.com/barisdinc/ESP32-DATV-SA#readme).
- For experienced users with Python: `pip install esptool`, then
  `esptool --chip esp32c3 write-flash 0x0 ESP32-DATV-SA-<version>.bin`
  (older esptool versions: `esptool.py --chip esp32c3 write_flash 0x0 …`).
  The `separate/` folder in the ZIP holds the bootloader (`0x0`), partition table (`0x8000`) and
  application (`0x10000`) as single files.

---

## Türkçe

Bu rehber, hazır ESP32-DATV-SA yazılımını yeni (sıfır) bir ESP32-C3 kartına yükler. **Hiçbir program
kurmanız gerekmez** — ne ESP-IDF, ne Python, ne sürücü. Windows, macOS ve Linux'ta yalnızca web
tarayıcısı yeterlidir.

Yüklemeden sonra kart, elektrik verildiği anda kendi kendine bir DVB-S2 test yayını yapar:
**2300.500 MHz, DVB-S2 QPSK 3/4, 333 kS/s, pilotlar açık, düşük seviye (−27 dB)**, servis adı `yATOLYE-TV` / `Technology Community`.

### Gerekenler

- USB girişli bir **ESP32-C3** kartı (örneğin ESP32-C3 SuperMini, 4 MB flash).
- **Veri taşıyan** bir USB kablosu (bazı ucuz kablolar sadece şarj eder; kart görünmezse kabloyu değiştirin).
- **Google Chrome**, **Microsoft Edge** veya **Opera** (masaüstü). Firefox ve Safari çalışmaz.
- [Son sürüm](https://github.com/barisdinc/ESP32-DATV-SA/releases/latest) sayfasından
  **`ESP32-DATV-SA-<sürüm>.bin`** dosyası (firmware ZIP'inin içinde de var).

### Adımlar

1. Sürüm sayfasından `ESP32-DATV-SA-<sürüm>.bin` dosyasını **indirin**.
2. ESP32-C3'ü bilgisayara **takın**.
3. Chrome veya Edge ile **<https://espressif.github.io/esptool-js/>** adresini açın
   (çipin üreticisi Espressif'in resmi web yükleyicisi).
4. *Baudrate* değerine dokunmadan **Connect**'e tıklayın. Açılan pencerede
   **USB JTAG/serial debug unit** portunu seçin (Windows: *COM* portu, macOS: `cu.usbmodem…`,
   Linux: `ttyACM0`) ve **Connect**'e basın.
5. *(Yeni kartta önerilir)* **Erase Flash**'a tıklayın ve bitmesini bekleyin.
6. *Program* bölümünde (dosya tablosu boşsa bir kez **Add File**'a tıklayın):
   - **Flash Address** değerini **`0x0`** yapın (varsayılan `0x1000` ESP32-C3 için yanlıştır),
   - yanındaki dosya düğmesiyle (*Dosya Seç* / *Choose File*) indirdiğiniz `.bin` dosyasını seçin.
7. **Program**'a tıklayın ve %100 olmasını bekleyin (yaklaşık yarım dakika).
8. Kartı çıkarıp tekrar takın (veya **RST** düğmesine basın). Kart artık yayında.

### Kendi videonuz (isteğe bağlı)

Kart, test yayını yerine flash belleğinde **`0x110000`** adresine yazılmış, en fazla **~35 saniyelik**
bir videoyu döngüde yayınlayabilir. Firmware değişmez: videoyu değiştirmek için yalnızca yeni video dosyasını yazarsınız.

1. Dosyayı Python 3 ve FFmpeg kurulu bir bilgisayarda hazırlayın (depodan, `pip install -r host/requirements.txt` sonrası):
   `python3 host/make_video.py filmim.mp4 --start 10 --seconds 20 -o video.bin`
   (`--start` ve `--seconds` alınacak bölümü seçer; bir sahne geçişinde başlayıp biten döngü, başa sarmadaki kısa takılmayı gizler).
2. esptool-js'de firmware'i yukarıdaki gibi yükleyin, sonra **Add File** ile ikinci bir satır ekleyin:
   **Flash Address `0x110000`**, dosya `video.bin`, ve **Program**'a tıklayın. Sonradan yalnızca videoyu değiştirmek için
   sadece bu satırı yükleyin; firmware'e dokunulmaz.
3. **RST**'ye basın. Kart videoyu döngüde yayınlar; ön panelde `VIDEO 20s SR331` görünür.
   **Alıcıyı 330.579 kS/s'e ayarlayın** (yalnızca tam sayı alıyorsa 330 veya 331): video, test yayınının 333 kS/s'inden
   biraz düşük bir sembol hızı kullanır. Frekans, seviye ve diğer ayarlar aynıdır.

**Erase Flash** videoyu da siler; kart yeniden test yayınına döner.
esptool ile yalnızca videoyu silmek için: `esptool --chip esp32c3 erase-region 0x110000 0x1000`.

### Sorun giderme

- **Port görünmüyor / bağlanamıyor:** Başka bir (veri) kablosu ve başka bir USB girişi deneyin.
  Sonra kartı yükleme moduna alın: **BOOT** düğmesini basılı tutun, **RST**'ye basıp bırakın
  (ya da BOOT basılıyken kabloyu takın), BOOT'u bırakın ve tekrar *Connect*'e tıklayın.
- **Linux'ta erişim reddedildi:** Bir kez `sudo usermod -aG dialout $USER` çalıştırın, oturumu kapatıp açın.
  *Snap* ile kurulmuş Chromium seri porta erişemez; Chrome veya Edge kullanın.
- **Windows 7/8'de sürücü yok:** Windows 10 veya 11 kullanın; orada ESP32-C3'ün USB portu sürücü istemez.
- **Yükledim ama sinyal yok:** Adres **`0x0`** olmalı. Yüklemeden sonra **RST**'ye basın.
  Alıcı ayarları: 2300.500 MHz, 333 kS/s, DVB-S2. Seviye bilerek düşük: antenleri yakın tutun.

### Notlar

- Beacon düşük seviyede (−27 dB) başlar; tam seviyede bile ESP32-C3'ün çıkışı birkaç miliwatt'tır.
  **PA enable** çıkışı (GPIO3) ön panelden açılmadıkça **kapalıdır**.
- İsteğe bağlı OLED / enkoder ön paneliyle frekans, seviye, sembol hızı ve isimler kart üzerinden
  değiştirilebilir ([ana README](https://github.com/barisdinc/ESP32-DATV-SA#front-panel)).
- Kart, ana projenin PC yazılımıyla da kullanılabilir: PC USB üzerinden komut gönderince beacon durur.
- 2.4 GHz'de yayın yapmak çoğu ülkede amatör telsizcilik lisansı gerektirir. Masa testlerinde
  zayıflatıcı veya yapay yük kullanın.
- Python bilen deneyimli kullanıcılar için: `pip install esptool`, ardından
  `esptool --chip esp32c3 write-flash 0x0 ESP32-DATV-SA-<sürüm>.bin`
  (eski esptool sürümlerinde: `esptool.py --chip esp32c3 write_flash 0x0 …`).
  ZIP'teki `separate/` klasöründe bootloader (`0x0`), bölüm tablosu (`0x8000`) ve
  uygulama (`0x10000`) ayrı dosyalar olarak bulunur.
