# Vývoj R600.4 CPU / GPU

GPU backend současného R600 je v `src/gpu_compute.cpp` a
`src/gpu_shaders.hpp`. Nepoužívá starý `reconstruction441_gpu.cpp`;
ten patří pouze historickému výzkumnému benchmarku. Profil GPU zachovává
současný formát a nezavádí starší enkodér ani rekonstrukci.

Udržuje se pouze současný kodek a jeho potřebné komponenty. Historické větve
se nevracejí do přepínače verzí ani do CLI. Aktivní build je `build-r4`.

[Architektura](docs/CURRENT.md), [nástroje a reference](tools/README.md).
