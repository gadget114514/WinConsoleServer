# WinConsoleServer

Windows 上で `bash` / `cmd.exe` / `powershell.exe` を **ConPTY** 経由で起動し、
LAN 上の専用ターミナルから TCP/IP で操作するためのサーバ + ターミナルアプリです。

- サーバ: `WinConsoleServer.exe`（コンソールアプリ）
- ターミナル: `WinConsoleTerminal.exe`（Native Win32 GUI）
- 独自バイナリプロトコル **WCSP v1.0**
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
  conpty.dll
  OpenConsole.exe
  tests\test_protocol.exe
  tests\test_frame_reader.exe

dist\
  WinConsoleServerSetup-0.2.0.exe
```

## 使い方

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
| `--max-sessions N` | 同時セッション上限 |
| `--allow-arbitrary-shell` | 許可リスト外のシェルを許可 |
| `--verbose` | 詳細ログ |

### 2. ターミナルから接続

```powershell
.\bin\Release\WinConsoleTerminal.exe --host 192.168.1.10 --port 7000 --shell cmd --connect
```

引数なしで起動すると接続ダイアログが表示されます。
接続設定は `%APPDATA%\WinConsoleTerminal.ini` に保存されます。

### 3. インストーラ

```powershell
.\dist\WinConsoleServerSetup-0.2.0.exe
```

サイレントインストール:

```powershell
.\dist\WinConsoleServerSetup-0.2.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

インストーラは任意で Windows ファイアウォールの TCP 7000 許可ルールを追加します。

## プロトコル (WCSP v1.0)

- TCP、1 接続 = 1 セッション
- 固定長整数はリトルエンディアン
- フレーム: `payload_len (u32) | type (u8) | flags (u8) | stream_id (u16) | payload`
- ペイロード上限: 1 MiB（既定）
- `HELLO` / `HELLO_ACK` は TLV
- 主なフレーム:
  - `INPUT`: クライアント → サーバの VT 入力
  - `OUTPUT`: ConPTY の VT/UTF-8 出力（無変換）
  - `RESIZE` / `RESIZED`: 画面サイズ同期
  - `EXIT`: 終了コードと理由
  - `ERROR`: エラーコードとメッセージ
  - `PING` / `PONG`: キープアライブ

詳細は [`doc/design.md`](doc/design.md) を参照してください。

## テスト

```powershell
ctest --test-dir build -C Release --output-on-failure
```

- `test_protocol`: フレーム/TLV/各メッセージのラウンドトリップ
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

- 1 接続 1 セッション（多重化は未実装）
- 切断時にシェルは終了（デタッチ / 再接続は未実装）
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
