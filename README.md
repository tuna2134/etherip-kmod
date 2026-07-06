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

TCP SYN に MSS オプションがある場合、外側 IPv6 経路の MTU を超えないよう、
送信時に IPv4/IPv6 の MSS を自動的に縮小します。802.1Q/802.1ad VLAN と IPv6
拡張ヘッダーにも対応します。既に十分小さい MSS は変更しません。

## トンネルの削除

```sh
sudo etherip-client delete -d eip0
```
