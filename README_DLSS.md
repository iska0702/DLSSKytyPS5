# KytyPS5 — DLSS & Frame Generation (Experimental Fork)

Экспериментальная сборка эмулятора **KytyPS5** с нативной поддержкой технологий **NVIDIA DLSS Super Resolution**, **DLSS 3 Frame Generation** и **NVIDIA Reflex**.

---

### Основные возможности

1. **NVIDIA DLSS Super Resolution (DLSS 3.7+)**:
   - Апскейлинг внутреннего рендера игры до выбранного целевого разрешения монитора (1080p, 1440p / 2K, 4K).
   - Интеллектуальная динамическая интерполяция и реконструкция деталей через тензорные ядра NVIDIA.
   - Управление резкостью (**DLSS Sharpness**) прямо из настроек конфигурации.

2. **NVIDIA DLSS 3 Frame Generation (Экспериментально)**:
   - Генерация промежуточных кадров на базе оптического потока (поддерживается на видеокартах **RTX 40-й серии**).
   - В интерфейсе оставлено простое включение стандартного DLSS 3 FG (2x).
   - *Продвинутый Multi-Frame Generation (MFG)* доступен только через CLI-параметр `--dlss-mfg <2x|3x|4x|dynamic>` для энтузиастов.

3. **NVIDIA Reflex Low Latency**:
   - Минимизация задержки ввода (режимы: *Off*, *On*, *On + Boost*).

4. **Патчи разрешения (Game Patches)**:
   - В папке `_Patches/PPSA01325.json` подготовлены готовые пресеты рендера для популярных разрешений:
     - **HD (720p)**: Ultra Perf (428x240), Perf (640x360), Balanced (744x432), Quality (854x480)
     - **FHD (1080p)**: Ultra Perf (640x360), Perf (960x540), Balanced (1114x648), Quality (1280x720)
     - **QHD / 2K (1440p)**: Ultra Perf (854x480), Perf (1280x720), Balanced (1486x840), Quality (1708x960)
     - **4K (2160p)**: Ultra Perf (1280x720), Perf (1920x1080), Balanced (2228x1252), Quality (2560x1440)
   - Все размеры кратны 2, что полностью исключает ошибку NGX `0xbad00005` (`InvalidParameter`).

---

### Как включить и проверить DLSS

1. В **Launcher**:
   - Зайдите в **Settings** -> **Configuration**.
   - Во вкладке **Graphics** установите целевое разрешение экрана (например, `2560x1440`).
   - В блоке **NVIDIA DLSS & Streamline (Experimental)** поставьте галочку `Enable DLSS Super Resolution`.
   - При наличии видеокарты серии RTX 40 включите `Enable Frame Generation (DLSS 3)`.
2. В патчах игры:
   - Откройте **Manage Patches** и выберите желаемый пресет внутреннего рендера (например, *QHD - DLSS Performance (1280x720)* или *DLSS Quality (1708x960)*).
   - Игра будет рендериться в этом разрешении, а DLSS восстановит четкое изображение до 2K/4K.
3. Проверка работы DLSS (Экранный оверлей NVIDIA):
   - Запустите файл `Enable_NVIDIA_DLSS_Indicator.reg`.
   - При запуске эмулятора в левом нижнем углу появится водяной знак NVIDIA DLSS с версией dll, исходным разрешением и режимом апскейла.
   - Для отключения запустите `Disable_NVIDIA_DLSS_Indicator.reg`.

---

### Требования к системе
- Windows 10/11 x64
- Видеокарта:
  - **NVIDIA GeForce RTX 20/30/40** (для DLSS Super Resolution и Reflex)
  - **NVIDIA GeForce RTX 40** (для DLSS Frame Generation)
- Актуальные драйверы NVIDIA Game Ready.
