# Magic Lantern Legacy Integration

SmoothAF is not a complete Magic Lantern tree. Integrate the module into a compatible Legacy Lua Fix source tree using the tree's module build conventions.

Required integration inputs:

- A compatible Legacy Lua Fix checkout.
- `platform/600D.102/magiclantern.sym` generated from the privately supplied `600D_102.SYM`.
- The module source and core header from `src/`.
- The generated `module_strings.h`.

The private camera symbol table is not stored in this repository. The build script accepts its path and creates only a derived, local symbol copy. Never substitute a table from another camera or firmware.

Do not modify `AUTOEXEC.BIN`, firmware, the loader, or an SD card as part of the build.
