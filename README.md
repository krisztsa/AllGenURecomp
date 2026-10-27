# ⚔️ AllGenURecomp — Monster Hunter Generations Ultimate Native PC Port 🐉🔥

**Port nativo para PC (Windows) de Monster Hunter Generations Ultimate (Nintendo Switch, v1.4.0) mediante recompilación estática de código (AOT — Ahead-of-Time), logrando rendimiento nativo en hardware moderno sin necesidad de emulación pesada.**

**Incluye un menú llamado KZMUI para que el jugador pueda acceder a las opciones mas fácilmente inspirado en el antiguo MH3UI de OceanTriRecomp.**




> ⚠️ **AVISO OFICIAL E IMPORTANTE:**
> **AllGenURecomp** es una recompilación estática de *Monster Hunter Generations Ultimate (NS)*, es **completamente gratuita** y si obtuviste este port de pago **has sido estafado**, la única vía oficial es nuestro GitHub: [https://github.com/krisztsa/AllGenURecomp](https://github.com/krisztsa/AllGenURecomp).

---

## 👑 Información del Proyecto y Créditos Oficiales

* **Creado por:** **Kris´ZtS**
* **GitHub del proyecto:** [https://github.com/krisztsa/AllGenURecomp](https://github.com/krisztsa/AllGenURecomp)
  *(Es un fork de [https://github.com/morerokk/MHGURecomp](https://github.com/morerokk/MHGURecomp) — creador de las herramientas)*
* **Mi canal de YT:** [https://youtube.com/@kriszts?si=la18fuis0frX-LGA](https://youtube.com/@kriszts?si=la18fuis0frX-LGA)
* **TikTok de mi contribuidor Alex Hunter:** [https://vm.tiktok.com/ZS9yNSKMqe2h9-2gYhm/](https://vm.tiktok.com/ZS9yNSKMqe2h9-2gYhm/)
* **Canal de Telegram del equipo:** [https://t.me/MHNativeProject](https://t.me/MHNativeProject)
* **Año de creación:** **Creado en 2026.**

---

## 🤖 Desarrollo, Arquitectura y Reconocimientos

Este proyecto nace como una adaptación y evolución personalizada (**AllGenURecomp**) siguiendo la misma filosofía y estructura modular de nuestro proyecto hermano de *Monster Hunter Tri* ([OceanTriRecomp](https://github.com/Kiridor123/OceanTriRecomp-Android)), pero llevada de lleno a **Monster Hunter Generations Ultimate**.

* **Asistencia de IA e Ingeniería (Claude 85%):** Con total transparencia, **Claude realizó aproximadamente el 85% del trabajo pesado** de traducción estática de instrucciones ARM de 32 bits (ARMv7 / NEON / VFP) a código C nativo, reimplementación de servicios del sistema de Nintendo Switch (`hle/`, `services/`), traducción de shaders Maxwell a SPIR-V/Vulkan y miles de optimizaciones de bajo nivel, junto con la dirección, pruebas intensivas, integración y ajustes personalizados de **Kris´ZtS**.
* **Proyecto Base y Herramientas:** Basado en el repositorio original [morerokk/MHGURecomp](https://github.com/morerokk/MHGURecomp) (creador de las herramientas base de recompilación `lifter.py`, `neon.py`, `gen.py` y el runtime NVN sobre Vulkan).
* **Tecnologías y Proyectos de Terceros:**

| Proyecto / Componente | Función en AllGenURecomp | Licencia |
| :--- | :--- | :--- |
| **AllGenURecomp Runtime & Lifter** | Traductor estático ARM32 → C, HLE de Switch y capa gráfica NVN → Vulkan | MIT |
| **Claude (Anthropic)** | ~85% del desarrollo de recompilación, refactorización, HLE y herramientas | — |
| **libSDL3** | Gestión multiplataforma de ventana, audio y mandos | zlib |
| **Vulkan / VulkanMemoryAllocator** | Renderizado gráfico nativo de alto rendimiento y gestión de VRAM | Apache-2.0 / MIT |
| **FFmpeg (`avformat`/`avcodec`/`avutil`)** | Decodificación nativa de cinemáticas y vídeo | LGPL / GPL |
| **Dear ImGui / cimgui** | Interfaz en pantalla (Overlay) y soporte para menús de plugins Forge | MIT |
| **LAS Recomp & Forge** | Base de componentes de runtime e inyección de plugins `.nro` | MIT |

---

## ✨ Novedades y Características Súper Guapas de AllGenURecomp

A diferencia de jugar en la consola original o mediante emulación tradicional, **AllGenURecomp** ejecuta el código del juego compilado directamente para tu procesador x86_64:

### 🖥️ 1. Gráficos de Nueva Generación y Ultra Alta Definición
* **Resoluciones Libres y Ultrawide:** Soporte nativo desde 720p/1080p hasta **1440p, 4K y monitores Ultrawide (21:9 / 16:10)**.
* **Escalado de Resolución 3D (`RenderResolution`):** Supersampling y ajuste independiente de la resolución interna 3D frente a la resolución de ventana.
* **Antialiasing Avanzado:** Soporte nativo para **MSAA (Off / 2x / 4x / 8x)** cambiable al vuelo con la tecla `F7`, además de **FXAA** opcional.

### ⏱️ 2. Tasa de Fotogramas Desbloqueada (30 a 240 FPS)
* **Framerate Nativo Configurable:** Juega a **30, 60, 90, 120, 144 o hasta 240 FPS** reales sin generación de fotogramas falsa y con físicas 1:1.
* **Control Total de VSync:** Activa o desactiva la sincronización vertical.
* **Avance Rápido de Cinemáticas:** Mantén pulsado el botón **A** para acelerar cualquier cinemática no omitible.

### 🌐 3. Multijugador LAN / Online Directo y Mejoras Cooperativas
* **Multijugador Local Wireless sobre IP:** Funciona directamente en red local (LAN / VLAN como ZeroTier o Radmin VPN) o por Internet abriendo el puerto `39500` (TCP/UDP).
* **Misiones de Aldea (Village Quests) en Multijugador:** Pulsa **L / R** en el mostrador de misiones del Hub / Cuartel para publicar misiones de la Aldea en cooperativo con tus amigos, con escalado automático de vida del monstruo (`HubHP = 2.0` por defecto).

### 🎮 4. Soporte Universal de Mandos y Teclado + Ratón
* **Compatibilidad Total con Mandos (SDL3):** Funciona de inmediato con mandos de Xbox, DualShock 4, DualSense (PS5), Nintendo Switch Pro Controller, Steam Deck y genéricos. Opción `FaceButtons = Label` o `Position` para intercambiar botones A/B y X/Y.
* **Controles Experimentales de Teclado y Ratón (KB+M):** Movimiento con `WASD`, cámara controlada con el ratón, sensibilidad ajustable (`MouseSensitivity`) y soporte directo de escritura/pegado (`Ctrl+V`) en el chat del juego.

### 🧩 5. Soporte Nativo de Mods, Plugins Forge y Trucos (Cheats)
* **Cargador de Mods Integrado (`mods/`):** Carga automática de mods `romfs` (como paquetes de texturas HD, *MHGU Rebalanced* o *Key Quest Markers*), parches `exefs` (`.pchtxt`, `.ips` de datos) y **plugins Forge (`.nro`)** con menú ImGui integrado (tecla `Insert`).
* **Motor de Trucos Compatible con Atmosphère/Ryujinx (`cheats/`):** Coloca archivos `.txt` con códigos de trucos de la versión 1.4.0. Incluye intérprete dinámico para trucos que modifican funciones del código en vivo y trucos extra en `extras/cheats/` (*Skill points x2*, *Valor Sheathe original damage reduction*).
* **Contenido Adicional Desbloqueado (`BonusContent`):** Incluye soporte para inyectar automáticamente todos los DLCs gratuitos (misiones de evento, packs de objetos y camaradas Felyne exclusivos) en tu partida.

---

## 🛠️ Requisitos del Sistema

| Componente | Especificación Mínima / Recomendada |
| :--- | :--- |
| **Sistema Operativo** | Windows 10 / 11 (64-bit) |
| **Procesador (CPU)** | Cualquier CPU de 2014 o superior con soporte **AVX2** (o compilación `x86-64-v2` para CPUs antiguos) |
| **Tarjeta Gráfica (GPU)** | Cualquier GPU de 2016 o superior compatible con **Vulkan 1.2** y al menos **1 GB de VRAM** |
| **Memoria RAM** | 4 GB de RAM o superior |
| **Archivos del Juego** | Copia legal propia de **Monster Hunter Generations Ultimate v1.4.0** (`exefs` y `romfs` extraídos) |

> 📌 **Identificadores requeridos de tu copia v1.4.0:**
> * **Title ID:** `0100770008DD8000`
> * **Build ID del ejecutable principal (`main`):** Empieza por `FB08F1D20FD1204F`
> *(Este repositorio NO incluye ni distribuye ningún archivo propietario del juego, claves ni enlaces de descarga).*

---

## 📥 Guía de Instalación Paso a Paso

1. **Descarga la última versión:** Ve a la sección de [Releases de AllGenURecomp](https://github.com/krisztsa/AllGenURecomp/releases) y descarga el archivo `.zip` correspondiente a tu plataforma (Windows).
2. **Extrae el port:** Descomprime el archivo en la carpeta que prefieras, por ejemplo `C:\Games\AllGenURecomp\`.
3. **Prepara los archivos de tu juego (v1.4.0):** Necesitas las carpetas `exefs` (de la actualización 1.4.0) y `romfs` (el juego base fusionado con la actualización 1.4.0) extraídas de tu Nintendo Switch o desde Ryujinx (*clic derecho en el juego con el update 1.4.0 aplicado → Extract Data*).
4. **Coloca los datos en la carpeta `data/`:** Copia `exefs` y `romfs` dentro de la subcarpeta `data` del port para que quede exactamente con esta estructura:

```text
AllGenURecomp\
  allgenurecomp.exe
  allgenugame.dll
  (otros archivos .dll)
  data\
    exefs\
      main
      main.npdm
      rtld
      sdk
      subsdk0
    romfs\
      nativeNX\
      (el resto de archivos romfs del juego)
```

5. **¡A cazar!** Ejecuta `allgenurecomp.exe`. Si falta algún archivo o la versión no coincide con la 1.4.0, una ventana te indicará exactamente qué debes revisar.

---

## 📂 Ubicación de Archivos, Partidas y Configuración

| Elemento | Ruta / Ubicación | Descripción |
| :--- | :--- | :--- |
| **Configuración** | `allgenurecomp.ini` (junto al `.exe`) |Se genera automáticamente al iniciar por primera vez. Bórralo si deseas restaurar los ajustes de fábrica. |
| **Partidas Guardadas (Windows)** | `%APPDATA%\AllGenURecomp\save\` | Separadas de la carpeta del juego para que nunca pierdas tu progreso al actualizar el port. |
| **Partidas Guardadas (Linux)** | `~/.local/share/AllGenURecomp/save/` | Compatible 1:1 con partidas de Switch y Ryujinx (copia tus archivos `system` y `system_backup` dentro de `save_0\`). |
| **Registro (Log)** | `log.txt` (junto al `.exe`) | Diagnóstico detallado de inicio, mods cargados, trucos y errores. |
| **Capturas de Pantalla** | `shots\` (junto al `.exe`) | Pulsa `F12` en cualquier momento para guardar una captura. |
| **Mods** | `mods\` (junto al `.exe`) | Una carpeta por cada mod (se aplican por capas al iniciar). |
| **Trucos (Cheats)** | `cheats\` (junto al `.exe`) | Archivos `.txt` con códigos en formato Atmosphère/Ryujinx (añade `_` al inicio del nombre del archivo para desactivarlo). |
| **Caché de Shaders** | `cache\` (junto al `.exe`) | Se puede borrar de forma segura si actualizas drivers de GPU. |

---

## 🎮 Controles en PC

Cualquier mando compatible con SDL3 funciona automáticamente al conectarlo. Si utilizas teclado y ratón, la disposición predeterminada es la siguiente:

| Tecla / Ratón | Botón Equivalente en Switch |
| :--- | :--- |
| **W / A / S / D** | Stick Izquierdo (mantén `Shift Izquierdo` para caminar) |
| **Movimiento del Ratón** | Stick Derecho / Cámara (`Clic` en ventana para capturar ratón, `Esc` para liberar) |
| **Clic Izquierdo / Clic Derecho** | **X / A** |
| **Espacio / R** | **B / Y** |
| **Q / E** | **L / R** |
| **Tab / T** | **ZL / ZR** |
| **Enter / Backspace** | **Plus (+) / Minus (-)** |
| **Flechas de Dirección** | **D-Pad (Cruceta)** |
| **Ctrl / Alt** | **Clic de Stick Izquierdo / Derecho** |
| **Alt + Enter** | Alternar Pantalla Completa / Modo Ventana |
| **F7 / F12 / Insert** | Cambiar nivel de MSAA / Tomar captura / Abrir menú de plugins Forge |

---

## ⚙️ Resumen de Ajustes (`allgenurecomp.ini`)

* **`[Display]`:** `Mode` (`Borderless`, `Fullscreen`, `Windowed`), `Resolution` y `RenderResolution` (resolución interna 3D).
* **`[Anti-aliasing]`:** `MSAA` (`Off`, `2x`, `4x`, `8x`) y `FXAA` (`On`, `Off`).
* **`[Performance]`:** `FPS` (`30` a `240`) y `VSync` (`On`, `Off` — desactívalo si eliges más FPS que los Hz de tu monitor).
* **`[Controls]` y `[Mouse]`:** `FaceButtons` (`Position` o `Label`) y `MouseSensitivity` (`0.1` a `10`, por defecto `1`).
* **`[Multiplayer]`:** `Port` (`39500` por defecto), `Join` (IP del anfitrión fuera de tu red local) y `SyncSmallMonsters`.
* **`[Monster HP]` y `[Village Quests]`:** Multiplicadores de vida para 1–4 cazadores en el Hub, `VillageQuestsInHub` (`On` por defecto) y `HubHP` (`2.0`).
* **`[Bonus Content]`:** `BonusContent` (`On` por defecto) para copiar los DLCs gratuitos de la carpeta `bonus/` en tu segundo inicio.

---

## 🏗️ Compilar desde el Código Fuente (Build from Source)

El proceso de compilación traduce y compila el código ARM de tu propia copia de *Monster Hunter Generations Ultimate v1.4.0* directamente a C y genera los binarios nativos.

### 1. Preparar el Entorno en Windows (MSYS2 UCRT64 + Python)

1. Instala **MSYS2** en `C:\msys64` y abre la terminal **MSYS2 UCRT64**.
2. Instala el compilador Clang, LLD y las librerías de desarrollo necesarias:

```bash
pacman -S --needed make git \
  mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-shaderc \
  mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-vulkan-memory-allocator
```

3. Instala **Python 3.10+** para Windows y crea el entorno virtual para las herramientas del recompilador en la carpeta del proyecto:

```cmd
py -3 -m venv tools\venv
tools\venv\Scripts\pip install -r tools\requirements.txt
```

### 2. Recompilar y Construir `AllGenURecomp`

Desde la consola **MSYS2 UCRT64**, sitúate en la carpeta del repositorio y ejecuta:

```bash
# Paso 1: Traducir el código ARM del juego a C dentro de generated/ (~300 MB)
make gen GAME=/c/ruta/a/tu/juego

# Paso 2: Compilar el runtime y la librería del juego en build/
make -j8 GAME=/c/ruta/a/tu/juego
```

Esto generará la carpeta `build/` lista para jugar con `allgenurecomp.exe`, `allgenugame.dll`, las librerías `.dll` necesarias y enlaces duros en `build/data/`. Puedes iniciar el juego ejecutando `run-game.bat` para mantener tus archivos de configuración y mods organizados en `run/`.


## 🗂️ Estructura del Repositorio

| Carpeta | Contenido y Propósito |
| :--- | :--- |
| **`tools/`** | Recompilador estático: `lifter.py` (ARM a C), `neon.py` (NEON / VFP), `gen.py` (generación de módulos), `nso.py` y herramientas de verificación con Unicorn (`test_lift.py`). |
| **`runtime/`** | Runtime nativo en el host: estado de CPU, kernel e hilos, servicios HLE de Switch (`services/`, `hle/`), red (`net/`), renderizador NVN sobre Vulkan (`nvn/`), input, ajustes y soporte de mods/Forge. |
| **`docs/`** | Documentación técnica, estado de ingeniería inversa y notas de investigación. |
| **`extras/cheats/`** | Archivos de trucos listos para copiar en la carpeta `cheats/`. |

---

## ⚖️ Licencia y Aviso Legal

* **Licencia del Código:** El código propio de **AllGenURecomp** se distribuye bajo la licencia **MIT** (consulta el archivo `LICENSE` y `THIRD_PARTY_NOTICES` para los componentes incluidos como cimgui, Dear ImGui, LAS Recomp y Forge). La licencia no cubre el juego original ni el código recompilado generado a partir de los archivos del usuario (`allgenugame.dll` / `allgenugame.so`).
* **Descargo de Responsabilidad:** Este proyecto no está afiliado, respaldado ni patrocinado por Capcom Co., Ltd. ni por Nintendo Co., Ltd. *Monster Hunter* es una marca registrada de Capcom. Este repositorio no contiene código del juego, activos gráficos, audio ni claves criptográficas; requiere que el usuario proporcione su propia copia legalmente adquirida.
* **Recordatorio:** **AllGenURecomp es 100% gratuito.** Si alguien intentó venderte este port o cobrarte por su descarga, repórtalo inmediatamente. Únete a nuestra comunidad oficial en [Telegram (@MHNativeProject)](https://t.me/MHNativeProject) y revisa siempre el repositorio oficial en [GitHub (krisztsa/AllGenURecomp)](https://github.com/krisztsa/AllGenURecomp).
