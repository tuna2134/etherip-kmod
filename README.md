# etherip-kmod

Linuxで使えるEtherIP(RFC3378)のためのカーネルモジュール

## 注意

Codex GPT-5.5を用いて開発し、途中から人間の手を入れてデバッグ作業を行いました。

粗削りなところは否めないため、自己責任でお願いします。

## ビルド

Arch Linuxの場合

```sh
sudo pacman -S linux-headers
make
```

## ロード

```sh
sudo insmod etherip6.ko
```

## 別途：クライアントの導入

ip-route2が対応していないため、独自のクライアントを導入する必要があります。
以下のリポからとってビルドしてください。
https://git.neody.ad.jp/tuna2134/etherip-client

## トンネルの作成

```sh
sudo etherip-client add -d eip0 -l 2001:db8::1 -r 2001:db8::2 -L eth0
sudo ip link set eip0 up
sudo ip addr add 192.0.2.1/30 dev eip0
```

`eip0` は TCP/UDP/SCTP の software GSO に対応します。GSO パケットは内側の
Ethernet フレームとしてセグメント化してから、それぞれを EtherIP でカプセル化
します。

## MTU と IPv6 フラグメント

EtherIP over IPv6 では、内側 Ethernet ヘッダー 14 bytes、EtherIP ヘッダー
2 bytes、外側 IPv6 ヘッダー 40 bytes の合計 56 bytes が追加されます。そのため、
`eip0` と外側インターフェースの MTU がともに 1500 の場合、最大サイズの内側
フレームは外側で 1556 bytes となり、IPv6 フラグメントが必ず発生します。

フラグメントの一部が欠落した場合、受信側ではパケット全体を再構成できません。
再構成の状態は次のコマンドで確認できます。

```sh
nstat -az | grep Ip6Reasm
```

`Ip6ReasmFails` または `Ip6ReasmTimeout` が転送中に増える場合は、フラグメントの
欠落、または受信側の再構成キュー不足が発生しています。再構成キュー不足に対する
緩和策として、受信側で次の値を設定できます。

```sh
sudo sysctl -w net.ipv6.ip6frag_high_thresh=268435456
sudo sysctl -w net.ipv6.ip6frag_time=10
```

この設定は経路上でのフラグメント欠落を防ぐものではありません。フラグメントを
避けるには、外側インターフェースと経路の MTU を 1556 以上にするか、`eip0` の
MTU を外側 MTU から 56 引いた値以下（外側 MTU 1500 なら 1444 以下）に設定して
ください。

一方、`eip0` の MTU は最大 9000 に設定できます。外側経路の MTU より大きい
非 GSO パケット、および GSO の各セグメントは、外側 IPv6 でフラグメント化して
送信されます。したがって MTU 9000 のフレームも転送できますが、経路上で IPv6
フラグメントが欠落しないことと、受信側に十分な再構成キューが必要です。

## トンネルの削除

```sh
sudo etherip-client delete -d eip0
```
