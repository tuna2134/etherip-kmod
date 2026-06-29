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

## トンネルの作成

```sh
sudo ./etherip6ctl add eip0 local 2001:db8:1::1 remote 2001:db8:1::2
sudo ip link set eip0 up
sudo ip addr add 192.0.2.1/30 dev eip0
```

例：

```sh
sudo ./etherip6ctl add eip0 local 2001:db8::1 remote 2001:db8::2 link eth0
```

デバイスの削除：

```sh
sudo ./etherip6ctl del eip0
```