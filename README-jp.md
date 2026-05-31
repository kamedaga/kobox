# kobox

Linux の `.ko` カーネルモジュールを、libc ベースの shim と OS ごとの backend によって userspace で動かす runtime です。

## kobox とは？

kobox はプリコンパイル済みの Linux カーネルモジュール (`.ko`) を userspace process にロードします。Linux kernel symbol は互換 shim が解決し、実際の device access は Linux VFIO、PachaOS、OpenBSD などの backend に委譲します。

最初の開発ターゲットは Linux です。Linux なら native kernel driver との挙動比較と性能計測がしやすいため、loader / shim / backend の正しさを Linux 上で固めてから他 OS に移植します。

## 設計目標

- 既存の `.ko` binary を再コンパイルなしで動かす。
- Linux 互換 shim は portable に保ち、libc ベースで実装する。
- OS 固有の device access は backend API の内側に閉じ込める。
- Linux、PachaOS、OpenBSD の対応を shim の書き換えではなく backend 実装にする。
- portable backend を増やす前に、Linux native driver との差分と overhead を測る。

## アーキテクチャ

```text
Linux Driver (.ko binary)
        |
        | Linux kernel symbols
        v
libc-based Linux shim layer
  kmalloc, mutex, workqueue, pci_*, dma_*, request_irq, ...
        |
        | kobox backend API only
        v
OS backend
  linux_vfio / pachaos / OpenBSD / FreeBSD...
```

shim 層はあえて libc と標準的な userspace primitive を使います。`malloc`, `pthread`, `mmap`, `clock_gettime`, C atomics などを使うことで実装量を減らし、libc が使える target OS に広げやすくします。

backend は OS 固有の device access だけを担当します。

- device enumeration
- PCI config access
- BAR/MMIO mapping
- DMA allocation / mapping
- IRQ delivery
- time, logging, event integration

## 現在のステータス

初期設計段階です。次の milestone は以下です。

1. architecture と backend API を固定する
2. ELF header/section 表示まで入った `kobox-inspect` を symbol / relocation 解析へ広げる
3. `linux_mock` backend を実装する
4. 最小 userspace module loader を実装する
5. 実 hardware 用に `linux_vfio` backend を追加する

この段階では versioning や release は意識しません。loader、shim の境界、backend API が固まるまでは、kobox は versioned runtime ではなく design/prototype project として扱います。

## Build

kobox は C11 で書き、CMake と clang で build します。

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

## ロードマップ

1. NVMe — 読み書きや負荷のある処理に対応完了💕
2. USB (xHCI) — USB HID が bind し、subsystem/input に device 登録
3. Network (e1000e / r8169) — PCI + DMA shim を流用
4. SATA (AHCI) — NVMe とストレージ shim を共通化
5. NVIDIA GPU — 一部重要な関数が未実装だが初期化に成功


## ライセンス

Apache-2.0
