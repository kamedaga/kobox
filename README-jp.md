# kobox

> Linux の `.ko` カーネルモジュールをユーザーランドで動かす — ポータブル・libc ベース・カーネル改変不要。

![Language: C11](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)
![Build: CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake)
![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

kobox はコンパイル済みの Linux カーネルモジュール (`.ko`) をユーザーランドプロセスとして動かすランタイムです。  
Linux カーネルシンボルは互換 shim 層で解決して、デバイスアクセスは OS ごとの backend (Linux VFIO / PachaOS Capsule など) に任せる構成になっています。

---

## 設計目標

- 既存の `.ko` バイナリを再コンパイルなしで動かす
- shim 層は libc だけに依存して移植性を保つ
- OS 固有のデバイスアクセスは backend API の内側に閉じ込める
- Linux・PachaOS・OpenBSD 対応は shim を書き換えるのではなく backend を追加する形で
- ポータビリティを語る前に、Linux ネイティブドライバとのオーバーヘッドをちゃんと測る

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
  linux_vfio / pachaos_capsule / linux_mock / ...
```

shim 層はわざと libc と普通のユーザーランド primitive (`malloc`, `pthread`, `mmap`, `clock_gettime`, C atomics) を使っています。内部でカーネルを再実装するつもりはなくて、libc が使える OS ならどこでも動かせるようにします。

backend が担当するのは OS 固有のデバイスアクセスだけ:

- デバイス列挙
- PCI config アクセス
- BAR / MMIO マッピング
- DMA バッファのアロケーションとマッピング
- IRQ デリバリー
- 時刻・ログ・イベント統合

---

## 現在のステータス

NVMe と USB は Linux VFIO・PachaOS Capsule の両 backend でエンドツーエンド動作済みです。

| Driver | Linux VFIO | PachaOS Capsule |
|---|---|---|
| NVMe | 動作中 | 動作中 |
| USB Storage (xHCI / BOT / SCSI) | 動作中 | 動作中 |
| Network (e1000e / r8169) | 進行中 | — |
| SATA (AHCI) | 予定 | — |
| NVIDIA GPU | `init_module` 通過まで確認済み | — |

---

## Build

C11 で書いていて、CMake と clang が必要です。

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

---

## PachaOS Capsule バックエンド

`pachaos_capsule` は PachaOS の `DeviceCapsule` トークンから kobox backend を作って、Capsule 操作は PachaOS ネイティブ syscall で行います。

```sh
KOBOX_PACHAOS_DEVICE_CAPSULE=0xca12000000000001 kobox-ls-devices pachaos
kobox-run --backend=pachaos --capsule=0xca12000000000001 run driver.ko
```

PachaOS Capsule ABI に config/BAR 情報取得が入るまでは、環境変数で PCI ID と BAR サイズを渡せます:

```sh
KOBOX_PACHAOS_PCI_ID=8086:10d3:02:00:00
KOBOX_PACHAOS_BAR0_SIZE=0x1000
```

---

## ロードマップ

1. NVMe — 完了
2. USB (xHCI) — 完了: HID + Mass Storage (BOT / SCSI / block I/O)、マルチデバイス対応
3. Network (e1000e / r8169) — PCI + DMA shim を流用して進行中
4. SATA (AHCI) — NVMe とストレージ shim を共通化して対応予定
5. NVIDIA GPU — 最終ボス (`init_module` まで通った)

---

## ライセンス

Apache-2.0
