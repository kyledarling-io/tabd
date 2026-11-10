# Third-party notices

tabd as a whole is GPL-3.0 ([LICENSE](LICENSE)) because it links moonlight-common-c. The vendored code below keeps its
own license. Each tree has a `SOURCE` file naming the exact upstream commit or release, and that tree has no local
changes.

| Component | Where | Upstream | License |
|---|---|---|---|
| moonlight-common-c | `native/third_party/moonlight-common-c/` | github.com/moonlight-stream/moonlight-common-c @ e80a786 | GPL-3.0, with an app-store distribution exception under section 7 ([LICENSE.txt](native/third_party/moonlight-common-c/LICENSE.txt)) |
| ENet (moonlight's fork) | `native/third_party/moonlight-common-c/enet/` | github.com/cgutman/enet @ aca8784 | MIT ([LICENSE](native/third_party/moonlight-common-c/enet/LICENSE)), Copyright (c) 2002-2020 Lee Salzman |
| nanors | `native/third_party/moonlight-common-c/nanors/` | github.com/sleepybishop/nanors @ a9b1636 | MIT ([LICENSE](native/third_party/moonlight-common-c/nanors/LICENSE)), Copyright (c) 2021 Joseph Calderon |
| Mbed TLS 3.6.2 | `native/third_party/mbedtls/` | github.com/Mbed-TLS/mbedtls, release tarball 3.6.2 | Apache-2.0 OR GPL-2.0-or-later ([LICENSE](native/third_party/mbedtls/LICENSE)); used here under Apache-2.0 |
| Rockchip MPP headers | `native/third_party/mpp/inc/` | github.com/rockchip-linux/mpp @ 37cc658 | Apache-2.0, Copyright Rockchip Electronics Co. LTD ([LICENSE](native/third_party/mpp/LICENSE)) |
| librga declarations | inside `native/tabd/tabd.c` (the RGA section) | github.com/airockchip/librga @ 7fd8d48 (v1.8.1), `im2d_type.h` and `im2d.h` | Apache-2.0, Copyright Rockchip Electronics Co. LTD |

At run time, tabd also loads the device's own vendor libraries (`libmpp.so`, `librga.so`) with `dlopen`. They belong
to the device and aren't part of this repo.

On the PC, the kit downloads Sunshine (GPL-3.0), the Virtual Display Driver by MTT, NefCon and NirSoft's
MultiMonitorTool from their own release pages (see [pc/README.md](pc/README.md)). None of them are redistributed here.
