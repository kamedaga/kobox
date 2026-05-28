# kobox

Linux の .ko カーネルモジュールを、ユーザー空間で、任意の OS 上でバイナリレベルで動かす 

## kobox とは？

kobox はプリコンパイル済みの Linux カーネルモジュール (.ko) をロードし、
カーネルシンボルを自前の shim 実装で解決することでユーザー空間で実行します。
カーネルソース不要。再コンパイル不要。.ko バイナリだけあれば動きます。

## なぜ kobox？

・ドライババイナリのみを実行するため、軽量シンプル。
・バイナリレベルで実行するため、クローズドドライバにも対応。
・Linuxの豊富な資源をユーザー空間で安全に実行可能

## アーキテクチャ

```
┌─────────────────────────────────┐
│     Linux Driver (.ko binary)    │
├─────────────────────────────────┤
│     kobox shim layer             │
│  (kmalloc, dma_*, pci_*, ...)   │
├─────────────────────────────────┤
│     Backend (OS 固有)            │
│  linux_vfio / pachaos / freebsd │
└─────────────────────────────────┘
```

## ロードマップ

1. NVMe — 単体 .ko、PCI + DMA + IRQ の shim 基盤構築
2. USB (xHCI) — 複数 .ko 同時ロード、サブシステム対応
3. Network (e1000e / r8169) — PCI + DMA shim を流用
4. SATA (AHCI) — NVMe とストレージ shim を共通化
5. NVIDIA GPU — 最終目標

## 設計思想

- **再発明しない — 再利用して隔離する**
- libc ベースの shim で最大限のポータビリティ
- バックエンド抽象化: shim 層は共通、OS ごとにバックエンドを差し替え
- Capability 対応: サンドボックス内でのドライバ実行を前提に設計

## 関連プロジェクト

- [PachaOS](https://github.com/kamerv/os) — Capability-based microkernel OS。kobox の主要ターゲット。

## ステータス

開発初期段階。ELF ローダーと shim アーキテクチャを設計中。

## ライセンス

Apache-2.0