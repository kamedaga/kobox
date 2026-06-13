# kobox

> Linux の `.ko` カーネルモジュールをユーザーランドで動かす — ポータブル・libc ベース・カーネル改変不要。

![Language: C11](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)
![Build: CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake)
![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

kobox はコンパイル済みの Linux カーネルモジュール (`.ko`) をユーザーランドプロセスとして動かすランタイムです。
Linux カーネルシンボルは Linux personality で解決し、OS 固有能力は platform facet と host interface に任せる構成にしていきます。

現在の実装は PCI-backed storage、USB、GPU などのデバイスドライバに強く寄っています。今後は同じ runtime で、filesystem、security module、sound、network、KVM 系 module などの主要 Linux module family も扱える構造に一般化します。

---

## 設計目標

- 既存の `.ko` バイナリを再コンパイルなしで動かす
- Linux 互換層は libc だけに依存して移植性を保つ
- OS 固有アクセスは platform facet と host interface の内側に閉じ込める
- デバイスアクセスは platform facet の 1 つとして扱い、runtime 全体の境界にしない
- Linux・PachaOS・OpenBSD 対応は Linux 互換層を書き換えるのではなく platform/interface を追加する形で
- ポータビリティを語る前に、Linux ネイティブドライバとのオーバーヘッドをちゃんと測る

## アーキテクチャ

```text
Linux Module (.ko binary)
        |
        | Linux kernel symbols
        v
Linux personality
  kmalloc, mutex, workqueue, VFS, driver model, LSM, ALSA, KVM, ...
        |
        | kobox runtime API only
        v
kobox runtime core
  lifecycle, object registry, event loop, timers, resources
        |
        +--> platform facets
        |      device, memory, event, time, log
        |
        +--> host interfaces
               socket, IPC, FUSE, sound, VM, ...
```

Linux personality はわざと libc と普通のユーザーランド primitive (`malloc`, `pthread`, `mmap`, `clock_gettime`, C atomics) を使っています。内部でカーネルを再実装するつもりはなくて、libc が使える OS ならどこでも動かせるようにします。

既存の device backend API は、現在の device platform facet です。担当するのは OS 固有のデバイスアクセスです:

- デバイス列挙
- PCI config アクセス
- BAR / MMIO マッピング
- DMA バッファのアロケーションとマッピング
- IRQ デリバリー
- 時刻・ログ・イベント統合

host interface は device backend とは別軸です。ロードした module を外部へ公開する OS 固有の面で、Linux socket、IPC、FUSE、ALSA/PipeWire bridge、PachaOS service endpoint などをここに実装します。

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
kobox-run --device=pachaos --capsule=0xca12000000000001 run driver.ko
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
5. NVIDIA GPU — `init_module` まで通った
6. Runtime 一般化 — platform facet、host interface、subsystem ごとの symbol 登録
7. 非ドライバ module family — filesystem を最初に、次に security、sound、KVM

### Runtime 一般化の進捗

- Phase 1: device / platform / host_interface / linux_personality / linux_subsystem の境界名へ整理済み
- Phase 2: exported symbol registry、module context、FS kernel object registry、fops smoke を loader から分離済み
- Phase 3: Linux symbol 登録を personality / subsystem provider へ分割済み
- Phase 4: platform facet の aggregate / accessor を実装開始

現在 provider 化済みの symbol family:

- `linux_core`
- `linux_stub`
- `block`
- `dma`
- `fs`
- `input`
- `kvm` (受け皿のみ)
- `net`
- `pci`
- `security` (受け皿のみ)
- `sound` (受け皿のみ)
- `usb`

`module_loader` は symbol table の所有者ではなく、各 provider を集約して module-local stub を配置する側に寄せています。loader に残る symbol は、loader-local static helper、tracepoint storage、x86 thunk、module patch 用の特殊 symbol だけです。

---

## ライセンス

Apache-2.0
