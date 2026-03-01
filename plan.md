# Linux Hybrid USB-over-WiFi Daemon — Geliştirme Planı

## TL;DR

Üç fazda inşa edilir. Faz 1 tamamen ağdan bağımsız — kernel tarafını (vhci-hcd, USB/IP parse, event loop) izole biçimde kurar ve stub ile test edilir. Faz 2 UDP + PassthroughHandler ekleyerek ESP32'ye gerçek bağlantı sağlar. Faz 3 descriptor cache ve optimized handler'larla performansı artırır.

---

## Dizin Yapısı (Faz 1 sonunda)

```
wirelessHub/
├── daemon/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── protocol/
│   │   │   └── Protocol.h             # header struct + CmdType enum (tanımlı, kullanılmaz)
│   │   ├── usbip/
│   │   │   ├── VhciDriver.h/.cpp      # /dev/vhci okuma/yazma, port attach/detach
│   │   │   └── UsbipParser.h/.cpp     # USB/IP → UsbRequest, UsbResponse → USB/IP
│   │   ├── core/
│   │   │   ├── DeviceRegistry.h/.cpp  # device_id → handler haritası
│   │   │   ├── Dispatcher.h/.cpp      # Faz 1: herkesi StubHandler'a yönlendirir
│   │   │   └── IHandler.h             # soyut arayüz
│   │   └── stub/
│   │       └── StubHandler.h/.cpp     # ağ yok; sabit/boş cevap döner, log basar
│   └── tests/
└── docs/
    └── daemon-plan.md
```

---

## Faz 1 — Yerel Altyapı (Ağsız)

### Adımlar

1. ✅ **`protocol/Protocol.h`**
   - `WirelessHubHeader` struct: `cmd_type (uint8_t)`, `endpoint (uint8_t)`, `seq (uint8_t)`, `length (uint16_t)` — 5 bayt, `__attribute__((packed))`
   - `CmdType` enum: `DEVICE_EVENT=0x10`, `UNOPTIMIZED_RAW=0x30`, `OPTIMIZED_DATA=0x20`, `ACK=0x40`, `ERROR=0xFF`
   - Bu fazda hiçbiri kullanılmaz; ilerideki fazlar için referans olarak durur

2. ✅ **`usbip/VhciDriver`**
   - Başlangıçta `modprobe vhci-hcd`
   - `/sys/devices/platform/vhci_hcd.0/attach` ve `detach` ile sanal port aç/kapat
   - `O_NONBLOCK` + `epoll` ile `/dev/vhci`'dan bloklanmayan istek okuma
   - Ham baytı `IUsbRequestHandler`'a iletir

3. ✅ **`usbip/UsbipParser`**
   - `parseSubmit` / `parseUnlink` — USB/IP → `UsbRequest`
   - `serializeSubmitReply` / `serializeUnlinkReply` — `UsbResponse` → USB/IP
   - `parseImportRequest` / `serializeImportReply` / `serializeImportError` — OP_REQ/REP_IMPORT
   - `parseDevlistRequest` / `serializeDevlistReply` — OP_REQ/REP_DEVLIST
   - `peekCommand()` — buffer'dan komut tipini okur

4. ✅ **`core/IUsbRequestHandler`** (IHandler → IUsbRequestHandler olarak yeniden adlandırıldı)
   - `virtual void handle(UsbRequest&, UsbResponse&)` soyut metodu
   - `virtual void onDetach()` soyut metodu
   - Tüm handler'lar bu arayüzü implemente eder

5. ✅ **`core/DeviceRegistry`**
   - `device_id → IHandler*` haritası
   - Thread-safe `add()` / `remove()` / `get()`

6. ✅ **`core/Dispatcher`**
   - Faz 1'de tek satır: her cihaz için `new StubHandler()` döner
   - Sınıf/subclass parametresi imzada var ama kullanılmıyor
   - Faz 2'de sadece bu dosya değişir
   - Sınıf/subclass parametresi imzada var ama kullanılmıyor

7. ✅ **`stub/StubHandler`**
   - `IUsbRequestHandler`'dan türer
   - `handle()` içinde: isteği log'a basar, `UsbResponse`'a boş/sabit veri yazar (NAK veya sıfır bayt)
   - Kaç istek işlendiğini sayar
   - Ağ kodu içermez
   - Faz 2 sonrasında da test aracı olarak kalır

8. ✅ **`main.cpp`**
   - `epoll` event loop: `/dev/vhci` fd
   - USB/IP isteği gelince: `UsbipParser` → `DeviceRegistry`'den handler → `StubHandler::handle()`
   - `SIGTERM` / `SIGINT` → tüm portlar detach, temiz çıkış

9. ✅ **`CMakeLists.txt`**
   - C++17, `-O2`, `-Wall -Wextra`, `pthread`
   - `make install` → `/usr/local/bin/wirelesshubd` + systemd unit taslağı

### Faz 1 Verification

- `wirelesshubd` başlatılır, test USB cihazı takılır
- `/dev/vhci`'dan isteğin okunduğu ve `StubHandler`'ın log'a bastığı doğrulanır
- Ağ bağlantısı gerekmez

---

## Faz 2 — UDP Haberleşmesi + Gerçek Bağlantı

### Eklenecekler

10. **`network/UdpSocket`**
    - ESP32 IP/port'una tek soket: `sendTo()` / `recvFrom()` + 2 saniyelik timeout
    - `seq` tabanlı basit ACK mekanizması (3 ardışık kayıp → cihaz offline)
    - Thread-safe gönderme kuyruğu (`std::queue` + `std::mutex`)

11. **`unoptimized/PassthroughHandler`**
    - `IHandler`'dan türer
    - Her gelen USB/IP isteğinin ağ overhead'ini soyar
    - `UNOPTIMIZED_RAW (0x30)` başlığıyla ESP32'ye gönderir
    - Cevabı alır, USB/IP formatına sarar, kernel'a teslim eder

12. **`Dispatcher` güncelleme**
    - `StubHandler` → `PassthroughHandler`
    - `StubHandler` silinmez, test aracı olarak kalır

### Faz 2 Verification

- `tcpdump` ile UDP paketleri izlenir
- `lsusb` ile sanal cihazın göründüğü doğrulanır
- `evtest` ile HID rapor akışı test edilir

---

## Faz 3 — Optimized Handler'lar

### Eklenecekler

13. **`core/DescriptorCache`**
    - Cihaz takılınca Device/Config/Interface descriptor'larını ESP32'den çekip RAM'e yazar
    - `get(device_id, descriptor_type)` ile sorgulama

14. **`optimized/BaseHandler`**
    - `handleControlTransfer()`, `handleDataTransfer()`, `onDeviceRemoved()` soyut arayüzü

15. **`optimized/` handler'ları**
    - `HidHandler` (0x03): GET_DESCRIPTOR cache'den, Interrupt IN ESP32'ye
    - `CdcHandler` (0x02): line-coding cache'den, Bulk ESP32'ye
    - `MassStorageHandler` (0x08): descriptor cache + SCSI Bulk ESP32'ye
    - `EthernetHandler` (0x02 alt sınıf): CDC-ECM descriptor cache + Ethernet frame Bulk

16. **`Dispatcher` genişletme**
    - `class 0x03` → `HidHandler`
    - `class 0x02` (alt sınıf kontrolü) → `CdcHandler` veya `EthernetHandler`
    - `class 0x08` → `MassStorageHandler`
    - Diğerleri → `PassthroughHandler`

### Faz 3 Verification

- Faz 2 vs Faz 3 UDP paket sayısı karşılaştırması (`tcpdump`)
- Cache hit/miss istatistiği log'a eklenir

---

## Kararlar

| Karar | Gerekçe |
|---|---|
| Stub faz zorunlu | Kernel tarafındaki hataları ağ karmaşıklığından bağımsız ayıklamak için |
| `StubHandler` kalıcı | Faz 2 ve 3'te regresyon testi için kullanılır |
   - `IUsbRequestHandler` arayüzü Faz 1'den itibaren | Faz geçişlerinde başka hiçbir dosyaya dokunmadan yeni handler eklenebilir |
| `Dispatcher` tek değişim noktası | Faz geçişlerinde sadece bu dosya güncellenir |
| UDP seçildi | HID/CDC için düşük gecikme öncelikli; kayıp koruması seq+ACK ile uygulama katmanında |
| C++ seçildi | `IHandler` kalıtım hiyerarşisi farklı cihaz sınıflarını temiz ve genişletilebilir tutar |
| Mass Storage Faz 3'te Mod A | SCSI komutları hala ESP32'ye gider ama descriptor round-trip ortadan kalkar |