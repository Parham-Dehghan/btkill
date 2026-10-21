# Build Instructions (Windows)

1. Open **Visual Studio 2019/2022**
2. Create a new project: **Console App (C++)**
3. Replace the content of the generated `.cpp` with `main.cpp`
4. Build: **Release | x64**
5. Run the exe **as Administrator**

## Notes
- All required libs (`ws2_32.lib`, `Bthprops.lib`) are auto-linked via
  `#pragma comment` — no manual linker config needed.
- If no Bluetooth adapter is found, the tool prints a full diagnosis
  with step-by-step fixes.