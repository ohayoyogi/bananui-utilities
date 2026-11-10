# J2ME アプリ管理

Bananui J2ME は `micromidp-linuxdev` の suite store を読み、登録済み MIDlet の一覧、起動、登録、削除を行う小さな管理画面です。アプリ一覧を別のデータベースへ複製せず、runtime が返す suite ID と MIDlet の順序を使います。

## ランタイムとの境界

現在対応するランタイムはmicromidpだけです。汎用のJ2ME管理規格や、別ランタイムでも動く共通プロトコルがあるとは扱いません。

- データフォルダはJAR/JADを管理アプリへ渡すだけで、micromidpのコマンドや登録形式を知りません。
- `src/j2me-app.c`は一覧・選択・確認画面と子プロセスの実行状態を扱います。ネイティブの応答行を直接解釈しません。
- `src/j2me.c`がmicromidp用の接続処理です。引数、応答形式、IDの検証、Waylandとapp IDの指定はここへ集約します。

別ランタイムへ対応する場合は、この接続処理と設定を追加・変更します。画面とデータフォルダは再利用する方針ですが、ランタイムごとの機能差に伴う画面調整まで不要とは保証しません。実装が一つの段階では、プラグイン機構や新しい共通サーバーは追加しません。

## 対応するランタイム

管理画面は、Wayland 表示と一覧・登録・起動・削除のコマンドを備えた `micromidp-linuxdev` を必要とします。対象ランタイムは `--list-suites`、`--install-only`、`--suite ID --midlet ORDINAL`、`--remove-suite ID`、Wayland 表示、`--app-id`、および version 1 の行プロトコルを実装している必要があります。古い runtime は一覧を読み込めず、利用可能な操作を始められません。

runtime 本体と MIDP 資材を用意し、実行ファイルは起動可能に、`lib-dir` は runtime が読む資材を含むディレクトリにしてください。runtime のビルド方法とオプションは microMIDP リポジトリの `docs/LINUX.md` と `docs/LINUX-DEVICE-LAUNCHER.md` を参照してください。

## 設定

`$XDG_CONFIG_HOME/bananui-utils/runtime.ini` に `[runtime]` セクションを作ります。`XDG_CONFIG_HOME` が未設定なら `~/.config` を使います。

```ini
[runtime]
executable=/absolute/path/to/micromidp-linuxdev
lib-dir=/absolute/path/to/runtime/lib
# data-dir=/absolute/path/to/j2me/data
# cache-dir=/absolute/path/to/j2me/cache
# keymap=/absolute/path/to/kyf31.keymap
# font=/absolute/path/to/japanese-font.bdf
```

`executable` と `lib-dir` は必須の絶対パスです。`data-dir` と `cache-dir` を省略すると、それぞれ `$XDG_DATA_HOME/bananui-utils/j2me` と `$XDG_CACHE_HOME/bananui-utils/j2me` を使います。ランタイムの画面表示には Wayland を使い、Bananui の画面と同じ `org.bananui.J2ME` app ID で起動します。

物理キーを操作する場合は、実行環境に合う`keymap`を指定してください。現在のmicromidpは未指定時に空の割当てになるため、画面が出てもキーで操作できません。PCでの確認にはruntime側の`targets/linuxdev/keymaps/pc-keyboard.keymap`を使います（Escapeが終話）。KYF31用には同ディレクトリの`kyf31.keymap`がありますが、今回変更版の実機入力は未検証です。

MIDlet内で日本語を表示する場合は、runtimeが対応する日本語BDFフォントを`font`に指定します。管理画面の日本語フォントとは別です。未指定時はruntime内蔵の欧文フォントを使います。

設定に誤りがある場合は画面に理由と設定ファイルの場所を表示します。設定を直した後、設定画面の F5／＃で再読込できます。一覧画面のF5／＃はアプリ一覧を更新します。

## 操作

- 一覧画面で上下キーを押すとアプリを選びます。決定キーで詳細を開きます。
- 詳細画面で上下キーを押すと MIDlet を選び、決定キーまたは左ソフトキーで起動します。MIDlet が終了すると一覧へ戻ります。
- 一覧画面の左ソフトキーからデータフォルダの JAR 選択画面を開けます。`.jar` を選ぶと登録確認が出ます。登録後は一覧を更新します。
- データフォルダから JAR を開くと管理画面へ渡します。ローカルの JAR が対象です。JAD、ネットワーク上のファイルは現在登録できません。登録を取り消すとファイル選択からやり直せます。
- 詳細画面の右ソフトキーまたは Delete キーで削除確認を開きます。削除すると MIDlet と RMS など suite に属する保存データも削除されます。
- 終話キーで画面を閉じます。MIDlet 実行中なら runtime に終了を依頼し、一定時間応答しない場合に停止します。

一覧の登録・削除は非同期で実行し、一定時間応答がない場合は処理を中止します。起動中の MIDlet は runtime の Wayland 表示ウィンドウで操作します。管理画面の操作は直列化されます。同じ suite store を別の runtime プロセスから同時に変更する使い方はサポートしません。

## ファイル形式と安全な再登録

ランタイムは `hello`、suite 行、MIDlet 行、件数、`bye` を含む version 1 の行プロトコルを返します。管理画面は UTF-8、件数、順序、suite ID、終端を検証し、不完全な一覧は表示しません。

ランタイムの従来のファイル名索引は同じ名前の異なる JAR を区別できないため、再利用前に suite ID の存在とインストール済み JAR の内容を照合します。異なる内容なら既存アプリや保存データを維持し、新しい JAR の登録をエラーにします。更新したい場合は別のファイル名で追加し、古い suite を削除してください。

## 開発者向け確認

utilities のビルド後、`meson test -C build` で保存・一覧応答・引数生成の試験を実行できます。Wayland の画面を含む結合試験は phoc セッションで行い、合成 runtime fixture による UI 動作と、実際の phoneME runtime による JAR 登録・一覧・起動・削除の確認を区別します。

## 確認結果（2026-11-10）

- phoc の合成 runtime 試験で、JAR 選択、登録確認、複数 MIDlet の選択、実行子プロセスの終了と管理画面への復帰、削除の取消・確定、通常のデータフォルダから既存管理画面への JAR 受け渡しを確認しました。
- 32-bit i386 phoneME と Wayland runtime をビルドし、実際の `size.jar` を AMS に登録しました。suite ID 2 と `SizeMIDlet` が一覧に現れ、選択起動で実際に描画し、EndCall で終了することを確認しました。phoc 上の管理画面でも登録、一覧、起動、EndCall による復帰、suite ID による削除を確認しました。
- `targets/linuxdev/test/smoke.py` は Size、RMS、AudioTest を含む全チェックが成功しました。同じファイル名で内容が異なる JAR を拒否し、元の suite が残ること、suite ID による削除、空一覧も確認しています。
- WSL 上では 58 件の utilities ASan/UBSan/リーク試験と `scripts/check.sh` が成功しました。linuxdev の 206 件の i386 unit checks は、keymap 資料を Linux のローカル `/tmp` に置いて実行した場合に成功しました。Windows マウント上では 32-bit のディレクトリ列挙が 1 件失敗したため、テスト対象資料をローカル `/tmp` に移して確認しています。

phoc の実画面試験には PC 用 keymap を指定しました（Escape を EndCall として扱います）。今回の画面試験では音声出力は確認しておらず、日本語フォントも設定していません。KYF31 の物理キー入力および実機動作はこの変更では確認していません。

`scripts/smoke-j2me.sh`は合成runtimeの画面試験です。実際のruntimeとの結合には、micromidp付属のSizeMIDletをビルドして次を実行します。Bananui shell、phoc、wtype、grim、D-Bus、ripgrepが必要です。runtimeのディレクトリは`micromidp-linuxdev`と`lib/`を含むものを指定します。既存のsuite storeは使わず、一時フォルダで登録・起動・削除します。

```sh
BU_TEST_PREFIX=PREFIX \
BU_MICROMIDP_RUNTIME=/absolute/path/to/runtime \
BU_MICROMIDP_SIZE_JAR=/absolute/path/to/size.jar \
BU_MICROMIDP_KEYMAP=/absolute/path/to/pc-keyboard.keymap \
sh scripts/smoke-j2me-native.sh
```
