# Peppe 🐕

Peppe è un assistente vocale AI che vive dentro un cagnolino robot: parla, ascolta, mostra le
emozioni su un piccolo display a colori e si muove sulle sue quattro zampe.

È una versione personale di [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) dedicata a **una
sola scheda: la ESP-HI**. Il progetto originale supporta oltre 130 board diverse; qui tutto ciò che
non riguarda ESP-HI è semplicemente rumore, quindi documentazione, comandi ed esempi parlano di
quella e basta.

Base di partenza: xiaozhi-esp32 v2.4.2. Il codice sorgente resta allineato all'upstream, così le
migliorie del progetto originale continuano ad arrivare (vedi [Restare allineati con
l'upstream](#restare-allineati-con-lupstream)).

---

## L'hardware

ESP-HI è un robot conversazionale open source a bassissimo costo progettato da ESP Friends, basato
su ESP32-C3. Schemi e sorgenti hardware sono sul [progetto LCEDA](https://oshwhub.com/esp-college/esp-hi),
e c'è un [video di presentazione su Bilibili](https://www.bilibili.com/video/BV1BHJtz6E2S).

| Componente | Dettaglio |
| --- | --- |
| MCU | ESP32-C3 (RISC-V, single core, **senza PSRAM**) |
| Flash | 4 MB |
| Display | ST7789 SPI a colori, 160×80 px (0,96") |
| Microfono | ingresso analogico su canale ADC 2 |
| Audio out | altoparlante PDM (GPIO 6 / 7), amplificatore abilitato da GPIO 3 |
| Movimento | 4 servo: anteriore sx/dx GPIO 21/19, posteriore sx/dx GPIO 20/18 |
| Pulsanti | BOOT GPIO 9, risveglio-movimento GPIO 0, risveglio-audio GPIO 1 |
| Rete | Wi-Fi 2.4 GHz |

La mappa completa dei pin sta in [config.h](main/boards/espressif/esp-hi/config.h), tutta la logica
della board in [esp_hi.cc](main/boards/espressif/esp-hi/esp_hi.cc).

La risorsa più scarsa qui è la memoria: il C3 non ha PSRAM e la flash è di 4 MB. Per questo la
configurazione di build ([config.json](main/boards/espressif/esp-hi/config.json)) taglia buffer
Wi-Fi, disabilita IPv6 e WPA3 e compila ottimizzando per dimensione. Se aggiungi funzionalità,
aspettati di dover togliere qualcos'altro.

## Cosa sa fare

- **Conversazione vocale** con modelli linguistici di grandi dimensioni, in streaming audio Opus
- **Wake word offline** tramite [ESP-SR](https://github.com/espressif/esp-sr): si sveglia da solo,
  senza premere niente
- **Espressioni animate** sul display, sincronizzate con il tono della conversazione
- **Movimenti**: il modello può comandare il corpo del cane, non solo rispondere a parole
- **LED RGB** controllabili a voce
- **WebUI** per pilotare i movimenti a mano, dal telefono

I movimenti sono esposti al modello come strumenti MCP, definiti in
[esp_hi.cc:302-358](main/boards/espressif/esp-hi/esp_hi.cc#L302-L358). Sono due gruppi:

- **Base** (`self.dog.basic_control`): avanti, indietro, gira a sinistra, gira a destra, stop
- **Estesi** (`self.dog.advanced_control`): dondolio avanti/indietro, sdraiarsi, dondolio laterale,
  ritirare le zampe, dare la zampa, stiracchiarsi, saltare in avanti

Vuoi insegnargli un movimento nuovo? Si aggiunge un `AddTool` accanto a quelli esistenti: il modello
lo scopre da solo, senza bisogno di riaddestrare o riconfigurare nulla lato server.

### WebUI

Con telefono e Peppe sulla stessa rete Wi-Fi, apri `http://esp-hi.local/` per controllare i
movimenti manualmente. Si disattiva da menuconfig, sotto
`Component config` → `Servo Dog Configuration` → `Web Control`.

## Cosa serve per lavorarci

- **ESP-IDF v6.0.2** (le versioni v6.0.x vanno bene; la v5.5 non è supportata per questa board)
- Python 3, incluso nell'installazione di ESP-IDF
- Un cavo USB-C **dati** — non uno di quelli da sola ricarica
- Linux o macOS. Anche Windows funziona, ma la compilazione è più lenta e i driver seriali danno più
  grattacapi

Prima di ogni sessione di lavoro, in ogni terminale nuovo:

```bash
. $IDF_PATH/export.sh
```

## Compilare

Usa sempre lo script di build: la ESP-HI ha una trentina di opzioni sdkconfig specifiche e lo script
le applica al posto tuo.

```bash
python ./scripts/build.py espressif/esp-hi
```

In italiano, con la wake word predefinita:

```bash
python ./scripts/build.py espressif/esp-hi --language it-IT
```

Le lingue disponibili sono in [main/assets/locales/](main/assets/locales/) — l'italiano c'è,
`it-IT`. Per l'elenco completo di lingue e wake word:

```bash
python ./scripts/build.py --list-languages
python ./scripts/build.py --list-wake-words
```

Attenzione a una regola non ovvia sulla wake word: l'ESP32-C3 può usare **solo i modelli "lite"**,
quelli con prefisso `wn9s_`. Il valore predefinito è `wn9s_nihaoxiaozhi` ("nǐ hǎo xiǎo zhì"). Se
passi un modello `wn9_` senza la `s` la build si ferma con un errore esplicito.

## Caricare il firmware

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

Sostituisci la porta con la tua: su macOS la trovi con `ls /dev/cu.*` a scheda collegata (è una
`usbmodem`, perché il C3 espone una USB Serial/JTAG nativa). Omettendo `-p`, idf.py prova a
indovinarla.

Il comando scrive bootloader, tabella delle partizioni, applicazione **e la partizione `assets`**
con emoji e font — quest'ultima grazie a una regola in
[main/CMakeLists.txt:1339](main/CMakeLists.txt#L1339). Se invece flashi a mano con `esptool` la sola
applicazione, il display resta senza risorse grafiche.

### ⚠️ Il trucco per far vedere la scheda al PC

Sulla ESP-HI il controllo dei servo occupa fisicamente il connettore USB-C. Con il corpo collegato
il computer **non vede** la scheda. Per programmarla:

1. Stacca l'alimentazione e **lascia solo la testa**, scollegando il corpo.
2. **Tieni premuto il pulsante** mentre colleghi il cavo al PC: il C3 entra in modalità download.
3. Dopo il flash può servire staccare e riattaccare l'alimentazione.

Per lo stesso motivo la build predefinita ha la console disabilitata, quindi `idf.py monitor` non
mostrerà log. Per vederli devi attivare `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
(`Component config` → `ESP System Settings` → `Channel for console output`), rinunciando però al
movimento. È un aut-aut hardware, non una svista del firmware.

## Prima accensione

Al primo avvio Peppe non conosce la rete, quindi crea un proprio hotspot Wi-Fi. Collegati con il
telefono, scegli la tua rete e inserisci la password: la pagina di configurazione si apre da sola.

Poi il dispositivo mostra un codice di attivazione da registrare sul server. Di default punta a
[xiaozhi.me](https://xiaozhi.me), dove un account personale permette di usare gratuitamente il
modello real-time Qwen. Se preferisci un server tuo, il firmware parla due protocolli documentati:
[WebSocket](docs/websocket.md) e [MQTT + UDP](docs/mqtt-udp.md).

## Cancellare tutto e ripartire da zero

Impostazioni Wi-Fi, attivazione e preferenze vivono nella partizione NVS (offset `0x9000`, 16 KB —
vedi [partitions/v2/4m.csv](partitions/v2/4m.csv)). Un normale `flash` **non** la tocca: aggiorni il
software ma il dispositivo si ricorda tutto.

Per riportarlo davvero come appena uscito di fabbrica, cancella l'intera flash prima di scrivere:

```bash
idf.py -p /dev/cu.usbmodem1101 erase-flash
idf.py -p /dev/cu.usbmodem1101 flash
```

Se ti serve solo dimenticare la rete Wi-Fi, senza ricompilare né riflashare, cancella la sola NVS.
Applicazione e risorse grafiche restano intatte, ed è questione di secondi:

```bash
esptool -p /dev/cu.usbmodem1101 --chip esp32c3 erase-region 0x9000 0x4000
```

C'è anche una scorciatoia senza cavi: **premi il pulsante BOOT durante l'avvio**, mentre Peppe si
sta ancora accendendo, e rientra subito in modalità configurazione Wi-Fi senza cancellare niente
([esp_hi.cc:171-177](main/boards/espressif/esp-hi/esp_hi.cc#L171-L177)).

## Dove mettere le mani

| Voglio cambiare... | File |
| --- | --- |
| Pin, dimensioni display, orientamento | [config.h](main/boards/espressif/esp-hi/config.h) |
| Comportamento, movimenti, pulsanti, strumenti MCP | [esp_hi.cc](main/boards/espressif/esp-hi/esp_hi.cc) |
| Opzioni di compilazione (memoria, Wi-Fi, wake word) | [config.json](main/boards/espressif/esp-hi/config.json) |
| Espressioni sul display | [emoji_display.cc](main/boards/espressif/esp-hi/emoji_display.cc) |
| Catena audio (mic ADC + altoparlante PDM) | [adc_pdm_audio_codec.cc](main/boards/espressif/esp-hi/adc_pdm_audio_codec.cc) |

Il codice comune a tutte le board — protocolli, servizio audio, display, macchina a stati — sta in
[main/](main/), e le note per chi sviluppa in [AGENTS.md](AGENTS.md). Il progetto segue lo stile C++
di Google.

## Restare allineati con l'upstream

Questo repository ha due remote: `origin` è il fork personale, `upstream` è
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). Per portare dentro le novità del progetto
originale:

```bash
git fetch upstream
git merge upstream/main
git push
```

Questo README ha preso il posto di quello originale, che resta consultabile come
[README_upstream.md](README_upstream.md) insieme alle versioni in
[cinese](README_zh.md) e [giapponese](README_ja.md). Nei merge dall'upstream il file `README.md` è
l'unico punto in cui aspettarsi conflitti ricorrenti: la risoluzione giusta è quasi sempre tenere la
versione di Peppe.

## Documentazione utile

- [Aggiungere una board personalizzata](docs/custom-board.md)
- [Protocollo MCP](docs/mcp-protocol.md) e [uso di MCP](docs/mcp-usage.md)
- [Protocollo WebSocket](docs/websocket.md) · [MQTT + UDP](docs/mqtt-udp.md)
- [Provisioning Wi-Fi via BluFi](docs/blufi.md)
- [Migrazione a ESP-IDF 6.0](docs/esp-idf-6-migration.md)
- [Stile del codice](docs/code_style.md)
- [Generatore di risorse personalizzate](https://github.com/78/xiaozhi-assets-generator) — wake word,
  font, emoji e sfondi, modificabili online

## Licenza

MIT, come il progetto originale. Vedi [LICENSE](LICENSE).
