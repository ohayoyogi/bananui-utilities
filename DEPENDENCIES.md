# 依存ライブラリと検証基準

アプリ本体と共通UIライブラリは別々に管理する。libbananuiのソース自体はこのリポジトリに含めない。

## libbananui

2026-11-09のホスト試験で使用した基準：

- libbananui: `aa7edf259fb27036682d23556f6c92c5be6560b2`
- igel-interpreterサブモジュール: `4fcf8c8e4522209485454d343f14cacf6ea922a7`
- 上記libbananuiへ[入力の修正](patches/libbananui-input.patch)を適用。

これはユーザーの動作版から取得したソースと元コミット情報に基づく記録。修正を取り込んだフォークのURL・コミットはまだ確定していない。libbananui側の独立した履歴へ移した後は、そのコミットをここに記録する。

適用はlibbananuiのチェックアウト内で行う（`/path/to/bananui-utilities`はこのリポジトリの絶対パスに置き換える）：

```sh
git apply --check /path/to/bananui-utilities/patches/libbananui-input.patch
git apply /path/to/bananui-utilities/patches/libbananui-input.patch
```

パッチは`src/input.c`と`src/input_method.c`だけを変更し、公開API・ABIは変えない。UTF-8の削除・カーソル、空文字の範囲外参照、文字列の解放を修正する。上流への投稿・取込みは未実施。同等の修正が取り込まれた版では二重適用しない。

libbananuiとこのパッチのライセンスはLGPL-3.0-or-later。アプリのMITライセンスとは区別する。[パッチの著作権・ライセンス](patches/README.md)を参照。

## その他

Meson >= 0.56、Ninja、Cコンパイラ、pkg-config、GLib >= 2.74、GIO、Pango/Cairoがアプリのビルドに必要。libbananuiのビルドには同ライブラリ側の依存関係も必要。

表示はWayland、メモの共通入力はtext-input-v3と対応IMEを使用する。日本語のかな漢字変換はBananuiシェルの既存IMEで確認した。日本語表示には対応フォントも必要。

アプリはKYF31のデバイスノードや端末制御を直接扱わない。シェル、コンポジタ、機種固有の設定・実機導入手順はこのリポジトリの管理範囲外。
