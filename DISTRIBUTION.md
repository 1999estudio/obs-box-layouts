# Distribución de OBS Box Layouts

Cada sistema operativo necesita su propio binario. No cambies el nombre ni la estructura interna de los paquetes
generados.

## Artefactos generados

Al ejecutar el workflow **Dispatch** de GitHub Actions se generan:

- `obs-box-layouts-VERSION-windows-x64.zip`: paquete portable para Windows de 64 bits.
- `obs-box-layouts-VERSION-windows-x64-installer.exe`: instalador por usuario para Windows.
- `obs-box-layouts-VERSION-macos-universal.tar.xz`: plugin para Mac Intel y Apple Silicon.
- `obs-box-layouts-VERSION-macos-universal.pkg`: instalador de macOS cuando el empaquetado lo produce.
- Paquetes y archivo portable para Ubuntu x86_64.

## Publicar una versión con GitHub Actions

1. Crea un repositorio vacío en GitHub.
2. Sube el contenido de este proyecto, incluida la carpeta oculta `.github`.
3. Abre **Actions → Dispatch → Run workflow**, selecciona `build` y ejecútalo.
4. Al terminar, descarga los artefactos de los jobs Windows, macOS y Ubuntu.
5. Prueba cada paquete en una instalación limpia de OBS 32 antes de distribuirlo.

Para crear una publicación versionada, actualiza `version` en `buildspec.json`, confirma los cambios y crea un tag con
el mismo número, por ejemplo `0.7.0`. El workflow deja un borrador de GitHub Release con los paquetes y sus checksums.

## Windows

El instalador coloca el plugin sin permisos de administrador en:

```text
%APPDATA%\obs-studio\plugins\obs-box-layouts
```

El ZIP contiene la misma carpeta portable:

```text
obs-box-layouts/
├── bin/64bit/obs-box-layouts.dll
└── data/
    ├── box-layout.effect
    └── locale/
```

Para instalar el ZIP manualmente, copia la carpeta `obs-box-layouts` completa dentro de
`%APPDATA%\obs-studio\plugins\` y reinicia OBS.

El instalador se crea con Inno Setup 6. En GitHub Actions la dependencia se instala automáticamente. Para compilarlo
localmente necesitas Windows x64, Visual Studio 2022, CMake, PowerShell 7 e Inno Setup 6.

## macOS

El paquete Universal funciona tanto en Apple Silicon como en Intel. La instalación por usuario usa:

```text
~/Library/Application Support/obs-studio/plugins/
```

Para distribuir sin advertencias de Gatekeeper se necesitan certificados **Developer ID Application** y
**Developer ID Installer**, además de credenciales de notarización de una cuenta Apple Developer. Sin esas credenciales,
GitHub Actions puede producir un artefacto de prueba con firma ad hoc, pero macOS puede advertir al descargarlo.

## Escenas, playlists y medios

El plugin no incluye la colección de escenas ni los archivos multimedia. Exporta la colección desde OBS y copia por
separado imágenes y videos. Al importar en otra computadora, revisa rutas de archivos y vuelve a seleccionar cámaras,
capturadoras y dispositivos que no tengan el mismo identificador.

Mantén la misma versión principal de OBS usada para compilar el plugin. Para una nueva versión de OBS, actualiza y
prueba las dependencias declaradas en `buildspec.json` antes de publicar nuevos instaladores.
