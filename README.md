# Proyecto ESP-WROVER-FB

Proyecto basado en ESP-IDF para ESP32 (target principal: `esp32`). Incluye componentes personalizados (`esp_firebase`, `jsoncpp`) y manejo de sensores + WiFi + Firebase.

---
### 1. Versiones Recomendadas
| Componente | Versión sugerida |
|------------|------------------|
| ESP-IDF    | ESP-IDF v5.5.1   |
| Python     | Python 3.11.2    |

Ver tu versión:
```
idf.py --version
```

Si cambias de versión, actualiza esta tabla para mantener consistencia.

---
### 2. Estructura relevante
```
ESP-WROVER-FB/
  CMakeLists.txt
  sdkconfig              # Configuración compartida del proyecto
  components/            # Componentes propios / externos gestionados
  main/                  # Código principal
    Privado.example.h    # Plantilla credenciales (se versiona)
    Privado.h            # Real (IGNORADO)
```

---
### 3. Credenciales (`Privado.h`)
El archivo real `main/Privado.h` está en `.gitignore`. Debes crear uno a partir de la plantilla:
```
copy main\Privado.example.h main\Privado.h   (Windows)
cp main/Privado.example.h main/Privado.h      (Linux/macOS)
```
Luego edita valores (WiFi, Firebase, Geoapify, etc.).

Ejemplo:
```c
#pragma once
#define WIFI_SSID "TU_SSID"
#define WIFI_PASS "TU_PASSWORD"
#define FIREBASE_API_KEY "TU_API_KEY"
```

---
### 4. Clonar y configurar en Windows
1. Instalar misma versión de ESP-IDF (instalador oficial) y abrir "ESP-IDF PowerShell".
2. Clonar:
```
git clone https://github.com/IngeLCT/ESP-WROVER-FB-WF.git
cd ESP-WROVER-FB-WF
```
3. Crear credenciales:
```
copy main\Privado.example.h main\Privado.h
```
4. Seleccionar target y preparar:
```
idf.py set-target esp32
idf.py reconfigure
```
5. Compilar:
```
idf.py build
```
6. Flashear + monitor (ajusta COM):
```
idf.py -p COM4 flash monitor
```
Salir: `Ctrl+]`.

Listar puertos:
```
Get-CimInstance Win32_SerialPort | Select-Object Name,DeviceID
```

---
### 5. Clonar y configurar en Linux / macOS
1. Instalar ESP-IDF (clonar repositorio oficial y `install.sh`).
2. Exportar entorno:
```
. $HOME/esp/esp-idf/export.sh
```
3. Clonar:
```
git clone https://github.com/IngeLCT/ESP-WROVER-FB-WF.git
cd ESP-WROVER-FB-WF
```
4. Credenciales:
```
cp main/Privado.example.h main/Privado.h
```
5. Compilar y flashear:
```
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---
### 6. Comandos frecuentes
```
idf.py build                    # Compila
idf.py flash                    # Flashea último build
idf.py -p <PORT> flash monitor  # Flashea y monitor
idf.py monitor                  # Solo monitor
idf.py size                     # Tamaño bins
idf.py menuconfig               # Config interactiva
idf.py fullclean                # Limpieza total
```

---
### 7. Dependencias
`dependencies.lock` fija versiones de componentes (IDF Managed Components). No lo borres. `managed_components/` se genera automáticamente.

---
### 8. Flujo Git sugerido
```
git pull --rebase
git checkout -b feature/nueva-funcion
git add .
git commit -m "feat: agrega X"
git push origin feature/nueva-funcion
```
Crear Pull Request -> revisar -> merge a `main`.

---
### 9. Cambio de versión ESP-IDF
Cuando se actualice:
```
idf.py fullclean
idf.py reconfigure
idf.py build
```

---
### 10. Problemas comunes
| Problema | Causa | Solución |
|----------|-------|----------|
| Falta Privado.h | No copiaste plantilla | Copiar `.example` y editar |
| Error puerto | Puerto incorrecto | Ver puertos / permisos |
| Falla tras upgrade | Caché vieja | `idf.py fullclean` |
| No conecta Firebase | Credenciales mal | Revisar `Privado.h` |

---
### 11. Licencia
Este proyecto se distribuye bajo la licencia **MIT**. Consulta el archivo `LICENSE` para el texto completo.
```
Copyright (c) 2025 IngeLCT

Se concede permiso, libre de cargos, a cualquier persona que obtenga una copia
de este software y archivos de documentación asociados (el "Software"), para usar
el Software sin restricción, incluyendo sin limitación los derechos a usar, copiar,
modificar, fusionar, publicar, distribuir, sublicenciar y/o vender copias del Software,
y a permitir a las personas a las que se les proporcione el Software a hacer lo mismo,
con sujeción a las siguientes condiciones:

El aviso de copyright anterior y este aviso de permiso se incluirán en todas las
copias o partes sustanciales del Software.

EL SOFTWARE SE PROPORCIONA "TAL CUAL", SIN GARANTÍA DE NINGÚN TIPO, EXPRESA O
IMPLÍCITA, INCLUYENDO PERO NO LIMITADO A GARANTÍAS DE COMERCIALIZACIÓN, IDONEIDAD
PARA UN PROPÓSITO PARTICULAR E INCUMPLIMIENTO. EN NINGÚN CASO LOS AUTORES O TITULARES
DEL COPYRIGHT SERÁN RESPONSABLES POR NINGUNA RECLAMACIÓN, DAÑOS U OTRA RESPONSABILIDAD,
YA SEA EN UNA ACCIÓN DE CONTRATO, AGRAVIO O CUALQUIER OTRO MOTIVO, DERIVADOS DE,
O EN CONEXIÓN CON EL SOFTWARE O EL USO U OTROS TRATOS EN EL SOFTWARE.
```

---
### 12. Mejoras futuras
- Añadir tests (Unity) para sensores.
- Documentar endpoints Firebase.
- Script para crear `Privado.h` automático.

---
¿Necesitas otro ajuste? Pide y lo añadimos.
