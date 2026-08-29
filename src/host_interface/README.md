# Host Interfaces

`host_interface` exposes loaded modules to the outside host OS.

The first target is intentionally thin: every host-facing module surface is
connected through an IPC endpoint unless a concrete subsystem proves that it
needs a richer transport.

The interface layer should not hide subsystem semantics. Filesystems, security
modules, sound, and KVM-style modules keep their Linux-facing meaning in
`linux_subsystem`; the host interface only binds that subsystem to an endpoint.

Host interfaces are separate from device backends.  A filesystem module may
need a host interface without a device backend, while a PCI driver may need a
device backend without any external interface.

koboxの現在の構造ではsubsystemの硬さやno-opの整理、ネイティブスレッド、SMP非対応、最適化のためのスケジューリングがstubになってる、ライセンスが少し怪しい、ライブラリなのかデーモンなのかわかりにくいなど本質的に重要な部分が欠けているため、仮称kobox2として考え直したい

やりたいことはsubsystemを既存のlinuxのコアカーネル部分の実装を動的ライブラリとしてコンパイルし、そのsoは.koと一緒にkobox2自身とは隔離した別のメモリ空間で動作させます
そしてkobox2自体とfiledなどのdaemonはプロセスを分離してしまいます

そしてそれらはring bufferで通信します
重要な点は
- kobox2自身が持つ機能を小さくし、モジュールローダー、インターフェースとして動く
- ring bufferのロックを避け、速度を高める
- subsystemを動的ライブラリとしてコンパイルするプロジェクトは完全にリポジトリを分離しgpl v2で行う
- kobox2とsubsystemのring bufferのインターフェースは意図的に制限し、触れるべきものだけにする
- kobox2にlinux関係の構造体などをハードコードしない
- linuxカーネルのバージョンは固定する

### 例えばfiled2(仮称) + ext4 + nvmeだったら、

filed2 プロセス

↕ ring buffer

kobox2 プロセス

↕ ring buffer

kobox sandbox
- linux primitiveのso(kmallocやkthreadなど)
- fs subsystemの.so
  - `jbd2.ko` / `ext4.ko`などの抽象化、世話をする
- blk subsystemの.so
  - `nvme.ko` / `nvme-core.ko`の抽象化

多分このプロジェクトで時間がかかる厄介な部分はsubsystemをどこまで流用し、どこから再実装するかの境界ですかねえ
多少時間をかけてもlinuxの.koは堅牢なので、それを安定させる基盤にしたい

あとkoboxからの方針変更としてkoboxでは重要な機能を無効化するstubをある程度許容していたが、それを禁止する

かなり大変だしring buffer文化はpachaosにないため、時間がかかりますが結構面白いと思いませんか