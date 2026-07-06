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

MTUを省略した場合、Overlay MTUはUnderlayの経路MTUからIPv6、EtherIP、
Ethernetヘッダーの合計56 byteを差し引いた値に自動調整されます。例えば
Underlay MTUが1500の場合、Overlay MTUは1444になり、通常の送信では外側IPv6
フラグメントを生成しません。

明示的に大きなMTUを設定することもできます。その場合は、カプセル化後の
パケットが実際のUnderlay経路MTUを超えたときだけ送信元でIPv6フラグメントを
生成します。

```sh
sudo ip link set eip0 mtu 1500
```

## トンネルの削除

```sh
sudo etherip-client delete -d eip0
```
