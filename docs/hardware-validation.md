# Камеры Huawei MateBook E 2023 (DRR-WXX)

Настройка выполнена на `live@10.9.0.5`, MiniOS / Debian 13,
ядро `6.12.111+deb13-amd64`.

## Использование

Приложениям доступны две камеры:

| Камера | Устройство | Сенсор |
| --- | --- | --- |
| MateBook Front | `/dev/video42` | Hynix HI846, ACPI `HYV0846` |
| MateBook Rear | `/dev/video43` | Samsung S5K3L6, ACPI `SSE5364` |

Выход: YUYV, 1280×720, около 30 кадров/с. Проверены одновременно обе камеры,
захват через V4L2 и фронтальная камера через PipeWire. Снимки содержат реальное
изображение. Для задней камеры при открытии выполняется подбор положения
объектива DW9714 по контрасту; это начальная фокусировка, а не непрерывное
слежение за движущимся объектом.

Служба включает сенсор при открытии соответствующей камеры. После закрытия
последнего клиента и паузы в пять секунд сенсор освобождается. В состоянии
ожидания подтверждены `runtime_status=suspended` у IPU6 и выключенные тактовые
сигналы обоих сенсоров.

Сырые каналы IPU6 закрыты для пользователя `live` правилом udev. Их использует
системная служба, которая публикует две обычные V4L2-камеры. После обновления
правил перезапущен WirePlumber: его каталог содержит ровно два видеоисточника.

Предпросмотр:

```sh
cheese --device=/dev/video42
```

Заднюю камеру можно выбрать в настройках Cheese.

## Установленные компоненты

- `/usr/local/src/matebook-e-cameras/`: исходники модулей и аппаратные таблицы.
- `/lib/modules/6.12.111+deb13-amd64/updates/matebook-e-cameras/`:
  `ipu-bridge.ko`, `hi846.ko`, `s5k3l6-matebook.ko`, `dw9714.ko`.
- `/usr/local/lib/matebook-e-cameras/ipa/ipa_soft_simple.so`: модуль libcamera
  0.7.1 с моделями усиления HI846 и S5K3L6 и уровнем чёрного 64/1024.
- `/usr/local/lib/matebook-e-cameras/lib/`: libcamera 0.7.1 с исправлением
  остановки Simple pipeline. Запросы, у которых уже возвращены буферы, но ещё
  ожидаются метаданные IPA, отменяются перед завершением потока.
- `/usr/local/libexec/matebook-e-camera-bridge.py`: мост libcamera → V4L2.
- `/etc/systemd/system/matebook-e-cameras.service`: загрузка модулей.
- `/etc/systemd/system/matebook-e-camera-bridge.service`: системный мост.
- `/etc/modprobe.d/matebook-e-cameras.conf`: две камеры v4l2loopback.
- `/etc/udev/rules.d/72-matebook-e-camera-private.rules`: внутренние узлы IPU6.
- `/etc/udev/rules.d/85-matebook-e-cameras.rules`: публичные камеры.

Обе системные службы включены в автозапуск. Старый пользовательский вариант
службы моста удалён при переходе к системному.

Статус и перезапуск:

```sh
sudo systemctl status matebook-e-cameras.service matebook-e-camera-bridge.service
sudo journalctl -u matebook-e-camera-bridge.service
sudo systemctl restart matebook-e-camera-bridge.service
wpctl status
```

Для проверки внутреннего каталога libcamera от root:

```sh
sudo env LIBCAMERA_IPA_MODULE_PATH=/usr/local/lib/matebook-e-cameras/ipa cam -l
```

## Драйверы и аппаратная настройка

HI846 основан на драйвере Linux 6.12. Добавлены ACPI, питание INT3472 и режим
3280×2464 для входной тактовой частоты 19,2 МГц. Исправлено возвращение
положительного результата `pm_runtime_get_if_in_use()` из обработчика
управления: успешная запись настроек должна возвращать ноль.

S5K3L6 использует новый ACPI-драйвер с режимами 2072×1552 и 4208×3120.
В ipu-bridge добавлены оба ACPI ID. Регистровые таблицы получены из драйверов
HI846 и S5K3L6, уже установленных в Windows на этом устройстве. Раздел Windows
при извлечении был смонтирован только для чтения.

Первая попытка с универсальными таблицами HI846 привела к ошибке старта
потока и потере связи с устройством; пользователь перезапустил MiniOS.
После исправления возврата и перехода на аппаратные таблицы получены
стабильные цветные кадры. Ошибка ранней попытки не является успешным тестом.

Пересборка модулей для текущего ядра:

```sh
sudo make -C /usr/src/linux-headers-6.12.111+deb13-amd64 \
  M=/usr/local/src/matebook-e-cameras CC=gcc-14 -j4 modules
```

Исходники `camera_sensor_helper.cpp` для IPA и `simple.cpp` для остановки
потока сохранены рядом. Они относятся к
libcamera tag `v0.7.1`, commit `183e37362f57ff3ce7493abf0bc6f1b57b931f55`.
Для пересборки используется обычный Meson/Ninja с `-Dprefix=/usr
-Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=enabled`; целевые файлы —
`src/libcamera/libcamera.so.0.7.1`, `src/libcamera/base/libcamera-base.so.0.7.1`
и `src/ipa/simple/ipa_soft_simple.so`.

Проверка автозапуска после перезагрузки ещё не выполнена.

## M-Pencil 3

По подтверждению пользователя, это перо работает с данным планшетом в Windows.
В Linux устройство пера с координатами/давлением пока не создано.

Обнаружен отдельный контроллер `spi-GXTS7986:00`, ACPI-путь
`\_SB_.PC00.SPI2.STHP`. В штатной конфигурации ядра выключен
`CONFIG_HID_GOODIX_SPI`.

Собран актуальный upstream `hid-goodix-spi` с исправленными адресами команды
`0x10364` и HID-отчёта `0x22c8c`. Для DRR-WXX добавлено явное ACPI-сопоставление
reset GPIO (ресурс 0, линия 0, active-low). Probe получает reset GPIO, IRQ 171
и SPI 20 МГц, но проверка обмена по адресу `0x10000` возвращает восемь нулей
и завершается `-EINVAL`. HID-дескриптор и устройство ввода не получены.

Экспериментальный модуль пера снят из памяти и не установлен в автозапуск.
Его исходник сохранён как `hid-goodix-spi.c`. Для продолжения следует проверить
питание/выбор режима и инициализацию контроллера по реализации Windows:

- `Program Files/Huawei/HuaweiThpService/goodix_thp_drv.dll`;
- `THP_Service.dll`, `TSACore.dll`, `TSAPrmt.dll` в том же каталоге;
- `Windows/System32/drivers/gt_berlinb_thp_driver.sys`;
- `USBDriver.sys` для `USB\VID_12D1&PID_10B8&MI_01`.

Установка одного xserver-xorg-input-wacom проблему не решает: требуется
работающий транспорт и инициализация контроллера до уровня устройства ввода.
