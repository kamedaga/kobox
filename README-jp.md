# kobox

> Linux `.ko` カーネルモジュールをユーザー空間で動かす。カーネルパッチなし、カスタムカーネルなし、libc だけ。

![Language: C11](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)
![Build: CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake)
![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

kobox はコンパイル済みの `.ko` バイナリを普通のユーザー空間プロセスに読み込んで、Linux カーネルシンボルの呼び出しを受け止める。互換レイヤーは純粋な libc ベース — `malloc`、`pthread`、`mmap`、`clock_gettime`、C atomics — カーネルの再実装ではない。OS 固有の処理（デバイス・メモリ・イベント）はプラットフォームファセットの裏に押し込んであるので、Linux 互換レイヤー自体はポータブルに保てる。

---

## なぜ作るか

カーネルモジュールをカーネル外で動かすアプローチの多くは、パッチ済みカーネル・VM・カスタム OS ビルドのどれかが必要になる。kobox はそのどれも使わない。

賭けはシンプル：libc の上に十分に忠実な Linux パーソナリティを乗せれば、カーネルに触らずに実際の `.ko` バイナリを動かせる。

設計上の制約：

- 既存の `.ko` バイナリを再コンパイルなしで動かす。
- Linux 互換レイヤーは libc のみ、ポータブルに保つ。
- OS 固有のサーフェス（デバイスアクセス・IPC・ソケット）はプラットフォームファセットとホストインターフェースに分離 — 互換レイヤーに埋め込まない。
- デバイスアクセスはファセットのひとつ。ランタイム全体の境界ではない。
- Linux / PachaOS / OpenBSD のサポート追加 = プラットフォーム・インターフェース側の作業。Linux 互換レイヤーには触らない。
- ポータビリティを主張する前に、ネイティブ Linux ドライバーとのオーバーヘッドをベンチマークで測る。

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

Linux パーソナリティは意図的にカーネルではない — libc + pthread + C atomics + POSIX プリミティブの集合体。内部でカーネルをエミュレートするわけでも、特殊なメモリモデルを用意するわけでもない。モジュールのシンボル呼び出しが「どこかまともなところに着地する」だけの必要最低限。


ホストインターフェースはデバイスバックエンドとは別物。読み込んだモジュールを外の世界に見せる側 — Linux ソケット・IPC・FUSE・ALSA/PipeWire ブリッジ・PachaOS サービスエンドポイントなど。

---

## プラットフォームファセットとホストインターフェース

### プラットフォームファセット

プラットフォームはデバイスバックエンドと OS レベルの抽象化をひとまとめにしたもの。ファセットは4つ：

| ファセット | 提供するもの | デフォルト実装 |
|---|---|---|
| memory | ランタイム所有リソースの alloc/free | `malloc`/`free`（デバイスバックエンドがあればそちらに委譲）|
| time | `monotonic_ns`、`sleep_ns` | `clock_gettime(CLOCK_MONOTONIC)` + `nanosleep` |
| log | ホスト側ロギング | デバイスバックエンドに委譲 |
| event | `poll_once(timeout_ns)` | タイムアウト分スリープ（イベントループのプレースホルダ）|

Linux・PachaOS・OpenBSD それぞれのプラットフォーム実装は `src/platform/` 以下にある — OS に合うデバイスバックエンドとインターフェースを選んで組み合わせる。

### ホストインターフェース

ホストインターフェースは、読み込んだモジュールを外に見せる仕組み。デバイスバックエンドとは独立していて、`kb_platform_desc_t` 経由でプラットフォーム生成時に渡す。

各インターフェースには `subsystem` フィールドがあって、どのモジュールファミリを担当するかを示す — `"fs"`、`"sound"`、`"security"` など。インターフェースの vtable は以下：

| Op | やること |
|---|---|
| `bind(platform)` | ロード時にプラットフォームへ接続 |
| `unbind()` | 切断 |
| `poll(timeout_ns)` | 受信イベントを確認 |
| `dispatch(msg, size)` | インターフェース経由でメッセージを送る |

現在実装済みの種類は IPC ひとつ（`KB_INTERFACE_IPC`、`kb_linux_ipc_interface_create()` で作る）。PachaOS IPC インターフェースが次。



---

## 現状

NVMe・USB・ext4 は Linux VFIO バックエンドでエンドツーエンド動作済み。NVMe と USB は PachaOS Capsule バックエンドでも動作済み。

| ドライバ / モジュール | Linux VFIO | PachaOS Capsule |
|---|---|---|
| NVMe | 動作 | 動作 |
| USB Storage (xHCI / BOT / SCSI) | 動作 | 動作 |
| ext4 (over virtio-blk) | 動作 | — |
| KVM (Linux ゲスト、`/sbin/init`) | 動作 | — |
| Network (e1000e / r8169) | 進行中 | — |
| SATA (AHCI) | 予定 | — |
| NVIDIA GPU | `init_module` 通過 | — |

### KVM — kobox 上で Linux ゲストを動かす

実物の `kvm.ko` + `kvm-amd.ko` を kobox に読み込み、Linux `bzImage` ゲストを `/sbin/init` まで起動することに成功した。

- [x] `kvm.ko` + `kvm-amd.ko` を kobox 上でロード・動作
- [x] ゲスト Linux `bzImage` が virtio-mmio 経由で ext4 rootfs までブート
- [x] ゲスト内で `/sbin/init` が実行される

### ext4 over virtio-blk

`ext4.ko` をユーザー空間で動かし、kobox block サブシステム経由で QEMU virtio-blk デバイスに繋いでファイル操作が通るところまで確認済み。スタック全体が完全にユーザー空間で完結している。

```text
ext4.ko
  -> kobox FS/VFS shim
  -> kobox block subsystem
  -> kobox VFIO virtio-blk provider
  -> Linux VFIO
  -> QEMU virtio-blk PCI device
  -> ext4 image backing file
```

---

## ビルド

C11、CMake、clang。

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

---

## PachaOS Capsule バックエンド

`pachaos_capsule` は PachaOS の `DeviceCapsule` トークンを kobox バックエンドにラップし、Capsule 操作を PachaOS ネイティブシスコール経由で処理する。

```sh
KOBOX_PACHAOS_DEVICE_CAPSULE=0xca12000000000001 kobox-ls-devices pachaos
kobox-run --device=pachaos --capsule=0xca12000000000001 run driver.ko
```

PachaOS Capsule ABI がまだ config / BAR 情報の取得 API を持っていないので、それまでは環境変数で渡す：

```sh
KOBOX_PACHAOS_PCI_ID=8086:10d3:02:00:00
KOBOX_PACHAOS_BAR0_SIZE=0x1000
```

---

## ロードマップ

1. NVMe — 完了
2. USB (xHCI) — 完了：HID + Mass Storage (BOT / SCSI / block I/O)、マルチデバイス
3. ext4 (over virtio-blk) — 完了：ユーザー空間でフルファイル I/O
4. Network (e1000e / r8169) — PCI + DMA shim を流用
5. SATA (AHCI) — NVMe と storage shim を共有
6. NVIDIA GPU — `init_module` 通過確認済み、残りは未着手
7. ランタイム汎化 — プラットフォームファセット・ホストインターフェース・サブシステム所有のシンボル登録
8. ドライバー以外のモジュール対応 — ファイルシステム (ext4) 完了、KVM 完了 (Linux ゲストが `/sbin/init` まで起動)、次いでセキュリティ・サウンド

---

## License

Apache-2.0
