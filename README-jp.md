# kobox -  ko In The Box

Linux の `.ko` カーネルモジュールを、libc ベースの shim と OS ごとの backend によって userspace で動かす runtime です。

## kobox とは？

kobox は任意のOSに`.ko` binaryを動かすプロジェクトおよび技術です。

## ターゲット

・Linux vfio(QEMU Ubuntuおよび実機のUbuntu24.04.4で動作確認)
・PachaOS Capsule(QEMU PachaOSでの検証 実機はまだ)


## 設計目標

- 既存の `.ko` binary を再コンパイルなしで動かす。
- shimはlibc依存のみ。
- OS 固有の device access は backend API の内側に。
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
  linux_vfio / pachaos / FreeBSD / seL4...
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

NVMeおよびUSBをLinux vfioバックエンドで動かすことに成功していて、NVMeからPachaOs Capsuleバックエンドで動かすことに挑戦しています。(安定はしないがread/writeが成功)


## Build

kobox は C11 で書き、CMake と clang で build します。

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

## 代表的な対応ドライバ

1. NVMe — 対応完了
2. USB (xHCI) — 対応完了 v1: HID + Mass Storage(BOT/SCSI/block I/O)、multi-device 
3. Network (e1000e / r8169) — PCI + DMA shim を流用
4. SATA (AHCI) — NVMe とストレージ shim を共通化
5. NVIDIA GPU — 一部重要な関数が未実装だが初期化に成功



## ライセンス

Apache-2.0
