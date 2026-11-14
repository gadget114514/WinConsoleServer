# WinConsoleServer

Windows 上で `bash` / `cmd.exe` / `powershell.exe` を **ConPTY** 経由で起動し、
LAN 上の専用ターミナルから TCP/IP で操作するためのサーバ + ターミナルアプリです。

- サーバ: `WinConsoleServer.exe`（コンソールアプリ）
- ターミナル: `WinConsoleTerminal.exe`（Native Win32 GUI）/ `wct.exe`（CUI）
- 独自バイナリプロトコル **WCSP v1.2**（セッションのアタッチ / デタッチ、複数クライアントのサイズ同期対応）
- ターミナル描画・VT 解析には [Ecode](https://github.com/gadget114514/Ecode) のターミナル部品を使用

```
+-----------------------------+          TCP (WCSP)                 +------------------------------+
| WinConsoleTerminal.exe      | <=================================> | WinConsoleServer.exe         |
|  TerminalView (D2D/DWrite)  |   HELLO / INPUT / RESIZE / ...      |  Session                     |
|  TerminalEmulator           |   OUTPUT / RESIZED / EXIT / ...     |  ConPTY                      |
|  TerminalBuffer             |                                     |  bash / cmd / powershell     |
|  RemoteSession              |                                     |                              |
+-----------------------------+                                     +------------------------------+
```

## 特長

- ConPTY を使うため、シェルからは**ローカルのターミナルが接続しているように見える**
  （`isatty()`、ジョブ制御、色、TUI、`stty size` が動作）
- 生の VT/UTF-8 バイト列を長さ付きフレームで転送。telnet のような NVT 変換をしない
- ウィンドウリサイズを `RESIZE` / `RESIZED` で同期
- 出力は境界付きキュー + バックプレッシャで欠落させない
- 終了コード、切断、プロトコルエラーを専用フレームで通知
- Ecode のスクロールバック、選択、コピー/貼り付け、IME、マウス、OSC 8/52/1337、sixel を利用
- Native Win32 (Win32 API + Direct2D/DirectWrite)。Qt / .NET / Electron 不使用
- Inno Setup 6 によるインストーラ

## 動作環境

- Windows 10 1809 (Build 17763) 以降 / x64
- ビルド: Visual Studio 2022 (C++ デスクトップ開発)、Windows SDK、CMake 3.20 以上
- インストーラ: Inno Setup 6
- `bash` を使う場合: Git for Windows の `bash.exe`

## ビルド

```powershell
git clone https://github.com/gadget114514/WinConsoleServer.git
cd WinConsoleServer

# 構成 + ビルド + テスト
.\scripts\build.bat

# インストーラまで生成
.\scripts\build.bat -Installer
```

手動で行う場合:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\WinConsoleServer.iss
```

生成物:

```
bin\Release\
  WinConsoleServer.exe
  WinConsoleTerminal.exe
  wct.exe
  wcs-cli.exe
  conpty.dll
  OpenConsole.exe
  tests\test_protocol.exe
  tests\test_frame_reader.exe

dist\
  WinConsoleServerSetup-0.4.1.exe
```

## 使い方

既定の待受ポートは **7000** です（`WinConsoleServer.exe` / `WinConsoleTerminal.exe` / `wct.exe` / `wcs-cli.exe` 共通）。
別ポートを使う場合はサーバの `--listen` とクライアントの `--port` を同じ値にしてください。

### 1. サーバを起動

```powershell
.\bin\Release\WinConsoleServer.exe --listen 0.0.0.0:7000 --shell cmd
```

`bash` を使う場合:

```powershell
.\bin\Release\WinConsoleServer.exe --listen 0.0.0.0:7000 --shell bash
```

主なオプション:

| オプション | 説明 |
|------------|------|
| `--listen host:port` | 待受アドレス (既定 `0.0.0.0:7000`) |
| `--shell cmd\|powershell\|bash` | 既定シェル |
| `--cwd path` | 作業ディレクトリ |
| `--token secret` | 接続トークン |
| `--token-file path` | トークンをファイルから読み込む |
| `--max-sessions N` | 同時セッション上限 |
| `--history-bytes N` | ジョブごとのリプレイ保持量 (64 KiB〜64 MiB、既定 32 MiB) |
| `--allow-arbitrary-shell` | 許可リスト外のシェルを許可 |
| `--log-file path` | ログをファイルへ追記 |
| `--install-service` | Windows サービスとして登録 (管理者) |
| `--uninstall-service` | Windows サービスを削除 (管理者) |
| `--service-name name` | サービス名 (既定 `WinConsoleServer`) |
| `--service` | SCM から起動されたときに使用 (通常は直接指定しない) |
| `--verbose` | 詳細ログ |

### 2. ターミナルから接続

```powershell
.\bin\Release\WinConsoleTerminal.exe --host 192.168.1.10 --port 7000 --shell cmd --connect
```

引数なしで起動すると接続ダイアログが表示されます。
接続設定は `%APPDATA%\WinConsoleTerminal.ini` に保存されます。

メニューの **File** から操作します。ボタンはありません。

- **New PowerShell / cmd / bash session**: 新しいサーバ側シェルを開始
- **Attach to session...**: 保持中のセッション一覧から選択して再接続
- **Connect to remote...**: ホスト / ポート / トークンを指定して接続
- **Detach (keep session)**: 切断だけ行い、サーバ側ジョブを保持
- **Close session (terminate)**: `CLOSE` を送り、サーバ側ジョブを終了

### 3. CUI クライアント `wct`

```powershell
# 新しい cmd セッション (127.0.0.1:7000)
.\bin\Release\wct.exe

# 保持中のセッション一覧
.\bin\Release\wct.exe --list

# ジョブ #12 に再接続
.\bin\Release\wct.exe 12

# リモートサーバ
.\bin\Release\wct.exe --host 192.168.1.10 --port 7000 --token secret --attach 12
```

| キー | 動作 |
|------|------|
| `Ctrl+]` | デタッチして終了（サーバ側ジョブは保持） |
| `Ctrl+]` を 600 ms 以内に 2 回 | `CLOSE` を送ってセッションを終了 |

トークンは `--token` または環境変数 `WCS_TOKEN` で指定します。

### 4. セッション管理 `wcs-cli`

```powershell
# 保持中のセッション一覧
.\bin\Release\wcs-cli.exe list

# セッションを終了して一覧から削除
.\bin\Release\wcs-cli.exe delete 12 13
.\bin\Release\wcs-cli.exe delete --all

# 接続用トークンを設定ファイルに保存
#   %APPDATA%\WinConsoleServer\wcs-cli.ini
.\bin\Release\wcs-cli.exe set-token secret123

# トークンを自動生成して保存
.\bin\Release\wcs-cli.exe set-token

# Windows サービスのトークンファイルに書き込む (管理者)
.\bin\Release\wcs-cli.exe set-token --server secret123

# 任意のファイルに書き込む
.\bin\Release\wcs-cli.exe set-token --file C:\path\token.txt secret123
```

`--host` / `--port` / `--token` / `--token-file` を省略すると設定ファイルの値を使います。
既定は `127.0.0.1:7000` です。

### 5. Windows サービスとして実行

管理者 PowerShell で実行します。

```powershell
# インストール（トークン未指定なら自動生成）
.\bin\Release\WinConsoleServer.exe --install-service --listen 0.0.0.0:7000 --shell cmd

# 生成されたトークンを確認
Get-Content "$env:ProgramData\WinConsoleServer\token.txt"

# 状態確認 / 開始 / 停止
sc.exe query WinConsoleServer
sc.exe start WinConsoleServer
sc.exe stop WinConsoleServer

# アンインストール
.\bin\Release\WinConsoleServer.exe --uninstall-service
```

- サービスの既定ポートも **7000** です。
- トークンの指定方法は次の順で優先されます:
  1. `--token secret`（サービスの `ImagePath` に保存）
  2. `--token-file path`
  3. 未指定時は `%ProgramData%\WinConsoleServer\token.txt` に自動生成
  4. 実行時の環境変数 `WCS_TOKEN`
- サービスモードのログは既定で `%ProgramData%\WinConsoleServer\server.log` に追記されます（`--log-file` で変更可能）。
- サービスのトークンを後から変更する場合:
  ```powershell
  .\bin\Release\wcs-cli.exe set-token --server newsecret
  sc.exe stop WinConsoleServer
  sc.exe start WinConsoleServer
  ```

### 6. インストーラ

```powershell
.\dist\WinConsoleServerSetup-0.4.1.exe
```

サイレントインストール:

```powershell
.\dist\WinConsoleServerSetup-0.4.1.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

インストーラは任意で Windows ファイアウォールの TCP 7000 許可ルールを追加します。

## プロトコル (WCSP v1.2)

- TCP。1 接続が 1 つの WCSP セッションに対応
- 固定長整数はリトルエンディアン
- フレーム: `payload_len (u32) | type (u8) | flags (u8) | stream_id (u16) | payload`
- ペイロード上限: 1 MiB（既定）
- `HELLO` / `HELLO_ACK` は TLV
- 主なフレーム:
  - `INPUT`: クライアント → サーバの VT 入力
  - `OUTPUT`: ConPTY の VT/UTF-8 出力（無変換、256 KiB に分割）
  - `RESIZE` / `RESIZED`: 画面サイズ同期
  - `LIST_SESSIONS` / `SESSION_LIST`: 保持中セッションの一覧
  - `EXIT`: 終了コードと理由
  - `ERROR`: エラーコードとメッセージ
  - `PING` / `PONG`: キープアライブ

`SHELL` には `cmd` / `powershell` / `bash`、既存ジョブへ接続する `attach:<id>`、
一覧取得専用の `list`、削除専用の `delete:<id>` を指定できます。

- **デタッチ**: ソケットを閉じるだけ。サーバ側ジョブは保持され、`attach:<id>` で再接続可能
- **終了 / 削除**: `CLOSE` フレーム、または `delete:<id>` を送る。サーバ側 ConPTY ジョブが終了し、一覧から削除される
- 複数クライアントが同じジョブに接続でき、各自が履歴リプレイを受け取る
- `SIZE_SYNC` を両側が広告した場合、あるクライアントの `RESIZE` が他クライアントへ `RESIZED` (reason=`PEER`) として転送される（最後に操作したクライアントのサイズが有効）

詳細は [`doc/wcsp.md`](doc/wcsp.md) を参照してください。

## テスト

```powershell
ctest --test-dir build -C Release --output-on-failure
```

- `test_protocol`: フレーム/TLV/各メッセージ、`SESSION_LIST` のラウンドトリップ
- `test_frame_reader`: 1 バイトずつの分割受信、複数フレーム、上限超過

手動確認:

```bash
# bash 接続時
[ -t 0 ] && echo TTY_OK
stty size
tput cols; tput lines
```

## セキュリティ

**v1 は LAN 前提で、通信は平文です。** インターネットに公開しないでください。

- 待受を特定の NIC / サブネットに限定する
- Windows ファイアウォールで許可元を絞る
- `--token` を設定する
- `--allow-arbitrary-shell` は必要な場合のみ有効にする

TLS、認証方式の拡張、圧縮、多重化はプロトコル上予約されています。

## 既知の制約

- ワイヤ上の多重化 (`stream_id`) は未実装
- 保持セッションはサーバ終了時にすべて終了する
- サーバ設定は CLI のみ（INI は今後対応）
- クライアント受信側のバックプレッシャは今後強化予定
- `powershell` は実機確認中

## ディレクトリ構成

```
src/common/   プロトコル、フレームパーサ、ConPTY ラッパ
src/server/   サーバ本体、セッション、設定
src/client/   ターミナル本体、RemoteSession、Ecode ターミナル部品
third_party/  conpty.dll / OpenConsole.exe、libsixel
tests/        単体テスト
installer/    Inno Setup 6 スクリプト
doc/          設計書
```

## サードパーティ

- [Ecode](https://github.com/gadget114514/Ecode) のターミナル部品
- [libsixel](https://github.com/saitoha/libsixel) (MIT)
- `conpty.dll` / `OpenConsole.exe` (Windows Terminal / MIT)

## ライセンス

このリポジトリのライセンスは未設定です。第三者コードのライセンスは各プロジェクトに従います。
