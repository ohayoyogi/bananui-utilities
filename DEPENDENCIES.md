# 依存ライブラリと検証基準

アプリ本体と共通UIライブラリは別々に管理する。libbananuiのソース自体はこのリポジトリに含めない。

## libbananui

共通入力の修正が取り込まれた `ja-input` のコミット `1041201c3a02b79ba4a9cfae0b61396a62f4ddbd` を基準にする。ソースの取得には当該リポジトリへのアクセス権が必要。 [libbananuiの基準コミット](https://github.com/ohayoyogi/libbananui/commit/1041201c3a02b79ba4a9cfae0b61396a62f4ddbd)

libbananuiのチェックアウトで `git submodule update --init --recursive` を実行し、依存するigel-interpreterを揃えてからビルド・インストールする。基準コミットが指定するサブモジュールの版は `4fcf8c8e4522209485454d343f14cacf6ea922a7`。

libbananuiのライセンスはLGPL-3.0-or-later。

## その他

Meson >= 0.56、Ninja、Cコンパイラ、pkg-config、GLib >= 2.74、GIO、Pango/Cairoがアプリのビルドに必要。libbananuiのビルドには同ライブラリ側の依存関係も必要。

表示はWayland、メモの共通入力はtext-input-v3と対応IMEを使用する。日本語のかな漢字変換はBananuiシェルの既存IMEで確認した。日本語表示には対応フォントも必要。

アプリはKYF31のデバイスノードや端末制御を直接扱わない。シェル、コンポジタ、機種固有の設定・実機導入手順はこのリポジトリの管理範囲外。
