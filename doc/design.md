# WinConsoleServer / WinConsoleTerminal 設計書

- 版: 0.2 (ドラフト)
- 対象: Windows 10 1809 (Build 17763) 以降 / x64
- 言語: C++17
- 実装形態: **Native Win32 アプリケーション**（Win32 API + C++17。UI フレームワーク不使用）
- 関連資産: `D:/wsgit/Ecode/Application/Terminal` (ターミナル描画・VT エミュレータ)
- 関連する過去実装: `D:/wsgit/Ecode/utility/telnetd/mytelnetd.c` (失敗事例。第 2 章で分析)

---

## 1. 概要

Windows 上で `bash` / `cmd.exe` / `powershell.exe` を **ConPTY** 経由で子プロセスとして起動し、
LAN 上の専用ターミナルアプリから TCP/IP で接続してコンソールアプリとして操作できるようにする。

```
+-----------------------------+          TCP (独自プロトコル)          +------------------------------+
| WinConsoleTerminal.exe      | <====================================> | WinConsoleServer.exe         |
|                             |                                      |                              |
|  TerminalView (D2D/DWrite)  |   HELLO / INPUT / RESIZE / ...       |  Session (per connection)    |
|  TerminalEmulator           |   OUTPUT / RESIZED / EXIT / ...      |  ConPTY (HPCON)              |
|  TerminalBuffer             |                                      |  bash / cmd / powershell     |
|  RemoteSession              |                                      |                              |
+-----------------------------+                                      +------------------------------+
```

設計の要点:

1. **telnet は使わない。** ConPTY が出力する VT/UTF-8 バイト列を、独自のバイナリフレーミングでそのまま運ぶ。
2. **サーバはバイト転送と PTY 管理に専念する。** 端末エミュレーション・文字幅計算・入力エンコードはクライアントが行う。
3. **クライアントは Ecode のターミナル部品を再利用する。** 既存の `ConPtySession` 依存を `ITerminalIo` 抽象で切り離し、
   ローカル PTY 用とリモート接続用を差し替える。
4. **ウィンドウサイズはハンドシェイク時に確定し、以後は `RESIZE` / `RESIZED` で同期する。**
5. **v1 は LAN 前提・平文。** ただし認証・TLS・圧縮・多重化を後から追加できる拡張点をプロトコルに用意する。
6. **両アプリとも Native Win32 で実装する。** Win32 API / Winsock / Direct2D / DirectWrite を直接使用し、
   Qt・.NET・Electron・MFC などのフレームワークには依存しない。

### 1.1 目的

- Windows 上で動く対話シェルを、別 PC の専用ターミナルからローカル端末と同等に使えるようにする。
- `bash` から見て「ローカルのターミナルソフトが接続している」状態にする（`isatty()` が真、ジョブ制御・色・TUI が動く）。
- リサイズ、終了コード、切断、エラーを明示的に伝える。
- Ecode の描画・VT 解析・入力エンコード資産を最大限再利用する。

### 1.2 非目的 (v1)

- インターネット越しの公開、暗号化、ユーザー認証基盤。
- telnet / SSH クライアントとの相互運用。
- 1 接続での複数シェル多重化（プロトコルには `stream_id` を予約）。
- セッションの永続化・再接続（切断時はシェル終了が既定）。
- ファイル転送プロトコル（OSC 1337 の画像・ダウンロードはクライアント側機能として扱う）。

### 1.3 実装方針: Native Win32

両アプリとも **素の Win32 API** で実装する。Ecode のターミナル部品も Win32 + Direct2D/DirectWrite で
書かれているため、この方針とそのまま整合する。

| 領域 | 採用する API / 実装 |
|------|---------------------|
| エントリポイント | サーバ: `wmain`（コンソールサブシステム） / クライアント: `wWinMain`（Windows サブシステム） |
| ウィンドウ | `RegisterClassExW` / `CreateWindowExW` / `WndProc` / `DefWindowProcW` |
| メッセージループ | `GetMessageW` / `TranslateMessage` / `DispatchMessageW`、`PostMessageW` によるワーカースレッド→UI 通知 |
| メニュー / ダイアログ | リソース (`resource.rc`) + `LoadMenuW` / `DialogBoxParamW` / `CreateDialogParamW` |
| コモンコントロール | `InitCommonControlsEx`（v6 マニフェスト）、`STATUSCLASSNAMEW` など |
| 描画 | Direct2D 1.0 / DirectWrite（Ecode `TerminalView` を踏襲） |
| ネットワーク | Winsock 2.2 (`ws2_32.lib`) のみ。サードパーティ通信ライブラリ不使用 |
| 設定 | Win32 INI API (`GetPrivateProfileStringW` / `WritePrivateProfileStringW`) または自前のキー=値パーサ |
| ファイル / パス | `CreateFileW` / `GetModuleFileNameW` / `SHGetKnownFolderPath` |
| 文字列 | UTF-16 (`wchar_t`) を API 境界で使用。プロトコルと端末ストリームは UTF-8 |
| ログ | `OutputDebugStringW` + `CreateFileW` による追記。ETW は将来拡張 |
| DPI | マニフェストで Per-Monitor V2。必要に応じ `SetProcessDpiAwarenessContext` |
| テーマ | `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` を任意で使用 |
| CRT | `/MT`（静的リンク）で単体 exe として配布可能にする |

禁止・非採用:

- Qt / MFC / ATL / WTL / .NET / WinUI / Electron / WebView2 を UI 基盤にしない。
- 独自のクロスプラットフォーム抽象ライブラリを挟まない。Win32 を直接叩く。
- サーバに GUI を持たせない（コンソールアプリとして動作。将来サービス化する場合も UI なし）。

Native Win32 とする理由:

1. Ecode の `TerminalView` が既に Win32 子ウィンドウ + Direct2D であり、そのまま埋め込める。
2. 追加ランタイム・追加 DLL を減らし、配布と起動を単純にする。
3. ConPTY / Winsock / スレッド / ハンドルなど、必要な機能がすべて Win32 API で直接扱える。
4. メッセージループとワーカースレッドの境界を明示でき、UI スレッドと I/O スレッドの責務を分離しやすい。

---

## 2. 過去実装 (telnetd) の失敗分析と本設計への反映

`mytelnetd.c` は ConPTY の出力を telnet ストリームに流す構成で、ターミナル表示が壊れた。
原因は 1 つではなく、**「生の VT ストリーム」と「telnet の NVT/オプション交渉」を同一ストリームで扱ったこと**にある。
以下、コードから確定できる不具合と、状況から強く疑われる要因を分けて整理する。

### 2.1 コードから確定できる不具合

| # | 箇所 | 内容 | 表示への影響 |
|---|------|------|--------------|
| 1 | `send_all(..., "Windows Telnet Server (ConPTY)\r\n", 33)` | 実際の文字列長は 32 バイト。33 を渡しており、**文字列終端の NUL を 1 バイト送信**している | NUL を解釈する端末・パーサで表示崩れ。少なくとも不正なバイト列 |
| 2 | `send_all(..., "\r\n*** Authentication failed: invalid password ***\r\n", 52)` | 実際は 51 バイト。同様に NUL を 1 バイト余分に送信 | 同上 |
| 3 | `process_iac()` の `IAC SB` 処理 | サブネゴシエーションが TCP セグメント境界で分割されると、未完了のまま末尾まで読み飛ばす。次チャンクの先頭がシェル入力として解釈され得る | NAWS の取りこぼし、交渉バイトが入力に混入 |
| 4 | `process_iac()` の境界処理 | `IAC` がバッファ末尾に来た場合の扱いが場当たり的で、バッファ溢れ時は `remain = 0` で保留データを全破棄 | 入力欠落、交渉状態の不整合 |
| 5 | 出力バッファ | `out[8192]` に対し 4KB 読み。全バイトが `0xFF` の場合 8192 バイト必要だが、`o < sizeof(out)-2` で **末尾 2 バイトを黙って捨てる** | VT エスケープシーケンスが途中で切れ、端末状態が恒久的に壊れる |
| 6 | NVT の CR/LF 変換なし | telnet クライアントは Enter を `CR LF` または `CR NUL` で送る。両方をそのまま ConPTY へ渡す | Enter が二重改行になる、行編集が崩れる |
| 7 | ECHO 交渉 | サーバが `IAC WILL ECHO` を送る一方、ConPTY 側でもシェルがエコーする。クライアントが ECHO を拒否/無視すると二重エコー | 入力文字が二重表示、行が崩れる |
| 8 | 初期サイズ | 常に 80x25 で `CreatePseudoConsole` し、NAWS は後から届いたときだけ反映 | TUI が初回レイアウトを誤り、再描画されないアプリもある |
| 9 | 終了処理 | 出力スレッドが `send` でブロックしたまま、クライアントスレッドが 5 秒待って `ctx` を `free` する。出力スレッドが復帰すると解放済みメモリを使用 | 切断時にクラッシュ・出力破損 |
| 10 | ConPTY ローダ | `conpty.dll` から `CreatePseudoConsole` を探すが、Ecode 同梱の DLL は `ConptyCreatePseudoConsole` 形式 | 環境によってはロード失敗、または kernel32 依存 |
| 11 | `resolve_shell()` | `static char path[]` を共有しており、複数クライアント同時接続で競合 | 誤ったパスで起動・起動失敗 |

### 2.2 状況から疑われる要因

1. **クライアント端末と `TERM=xterm-256color` の不一致**
   Windows 標準の telnet クライアントは完全な VT100/256 色エミュレータではない。
   サーバが 256 色・カーソル制御・代替画面を前提とした出力を送ると、クライアント側で
   エスケープシーケンスが文字として表示されたり、カーソル位置がずれたりする。

2. **UTF-8 とクライアントのコードページの不一致**
   ConPTY の出力は UTF-8。Windows の telnet クライアントが OEM コードページ (CP932/CP437 など) で
   解釈すると、日本語・罫線文字が文字化けする。さらに**全角文字の表示幅の計算がずれ**、
   折り返し位置が崩れて画面全体が壊れて見える。これは日本語環境で特に起きやすい。

3. **telnet の line mode / local echo**
   クライアントが line mode のままの場合、1 文字ずつ送られず、行単位で送られる。
   サーバ側は ConPTY のエコーを前提にしているため、表示と入力のタイミングがずれる。
   local echo が有効なクライアントでは二重表示になる。

4. **出力バースト時のバイト欠落**
   上記 #5 と、`PeekNamedPipe` + 固定長バッファ + ブロッキング `send` の組み合わせで、
   高負荷時に VT シーケンスの一部が欠ける。**1 バイト欠けるだけで以降の端末状態は復旧不能**になる。

5. **NAWS の分割・欠落**
   telnet の NAWS は TCP 上で分割され得る。分割時に取りこぼすと、シェルの認識サイズと
   クライアントの画面サイズが永久に一致しない。

6. **端末状態の初期化不足**
   接続前にクライアント端末が持っていたスクロール領域・文字集合・カーソル状態を引き継ぐ。
   ConPTY 側は新しいコンソールのつもりで出力するため、初期表示が崩れることがある。

7. **制御情報とコンソール出力の混在**
   バナー・エラー・認証プロンプト・シェル出力が同じバイトストリームに混在する。
   クライアントは「これはエラー文字列」と区別できず、画面に残留する。

### 2.3 本設計での対策

| 失敗要因 | 本設計での対策 |
|----------|----------------|
| NVT 変換・IAC・オプション交渉 | telnet を廃止。長さプレフィックス付きバイナリフレームで VT バイト列を無変換転送 |
| 8 ビット透過・UTF-8 | フレームはバイナリセーフ。UTF-8 をそのまま運び、クライアントが UTF-8 デコーダで処理 |
| クライアント端末の能力差 | 専用クライアント (Ecode エミュレータ) に固定。機能はハンドシェイクで交渉 |
| ECHO / line mode | プロトコルに入力モード概念を持ち込まない。クライアントは常に文字単位の VT 入力列を送る |
| CR/LF 変換 | ConPTY の入出力バイト列をそのまま往復。改行変換は一切しない |
| 出力欠落 | 固定長バッファで切り捨てない。境界付きキュー + PTY 読み取り停止によるバックプレッシャ |
| エスケープ分割 | フレーム長で境界を明示。クライアントの VT パーサはチャンク分割を前提に状態を保持 |
| NAWS 分割 | ハンドシェイクで初期サイズを確定。以後は `RESIZE` / `RESIZED` の明示フレーム |
| サイズ不整合 | ハンドシェイクのサイズで ConPTY を起動。変更は ACK 付きで適用結果を返す |
| 制御情報の混入 | バナーやエラーは `ERROR` / `EXIT` フレーム。コンソール出力 (`OUTPUT`) と完全に分離 |
| 終了時のクラッシュ | 明示的な状態遷移 + スレッド join + RAII。所有権を曖昧にしない |
| 同時接続の競合 | セッションごとに独立した設定・シェル解決。グローバル可変バッファを廃止 |
| 端末状態の初期化 | クライアントは新規 `TerminalBuffer` で開始。必要ならサーバが初期化シーケンスを `OUTPUT` として送る |

**結論:** telnet は「汎用クライアントと相互運用するためのプロトコル」であり、
ConPTY の生 VT ストリームを運ぶには余計な変換と交渉が多すぎる。専用プロトコルにすることで、
「バイト列を絶対に落とさない」「境界を明示する」「制御とデータを分離する」を保証できる。

---

## 3. 用語

| 用語 | 意味 |
|------|------|
| セッション | 1 TCP 接続に対応する ConPTY + 子プロセスの組 |
| OUTPUT | ConPTY が生成した VT/UTF-8 バイト列 |
| INPUT | クライアントが生成した VT 入力バイト列 |
| 優先入力 | 端末エミュレータが生成する応答 (DSR/CPR/DA など)。通常のキー入力より先に PTY へ書く |
| バックプレッシャ | クライアントが遅いとき、PTY の読み取りを止めて子プロセス側を待たせる仕組み |
| ローカルモード | Ecode 内で `ConPtySession` に直接つなぐ従来動作 |
| リモートモード | `RemoteSession` 経由で WinConsoleServer につなぐ動作 |

---

## 4. システム構成

### 4.1 コンポーネント

| コンポーネント | 実行ファイル | 役割 |
|----------------|--------------|------|
| サーバ | `WinConsoleServer.exe` | TCP 待受、ハンドシェイク、セッション管理、ConPTY、入出力転送 |
| ターミナル | `WinConsoleTerminal.exe` | 接続 UI、`RemoteSession`、Ecode ターミナル描画・VT 解析 |
| 共通プロトコル | `wcs_protocol.lib` | フレーム定義、シリアライザ、パーサ、TLV |
| 共通 ConPTY | `wcs_conpty.lib` | ConPTY ラッパ (サーバ用)。Ecode `ConPtySession` を基に拡張 |
| Ecode 端末部品 | `ecode_terminal.lib` | `TerminalView` / `TerminalEmulator` / `TerminalBuffer` / `ImageManager` |

### 4.2 プロセス・スレッド構成

**サーバ:**

```
main thread
  └─ accept loop
       └─ Session (1 client = 1 Session)
            ├─ FrameReader thread   : socket → フレーム解析 → PTY 入力 / 制御
            ├─ ConPTY output thread : PTY 出力 → 境界付きキュー
            └─ SocketWriter thread  : キュー → OUTPUT フレーム送信
```

- ConPTY 読み取りとソケット送信を分離し、**入力 (Ctrl+C 等) が出力バーストに巻き込まれない**ようにする。
- 送信キューが高水位を超えたら ConPTY の読み取りを一時停止する (バックプレッシャ)。
- 1 セッションの障害が他セッションに影響しないよう、スレッドと状態を完全に分離する。

**クライアント:**

```
UI thread (Win32 message loop)
  ├─ TerminalView (描画 / 入力 / VT エミュレータ)
  └─ RemoteSession::FeedOutput 呼び出し
RemoteSession
  ├─ Receiver thread : socket → フレーム → 受信キュー → UI へ wake
  └─ Sender thread   : 入力キュー → INPUT / RESIZE フレーム送信
```

### 4.3 データの所有権

- `TerminalBuffer` / `TerminalEmulator` は UI スレッドのみが触る。
- 受信スレッドは**コピーしたバイト列をキューに入れるだけ**。UI スレッドが取り出して `FeedOutput()` に渡す。
- 送信側は UI スレッドがキューに積み、Sender スレッドが送る。UI スレッドはソケット I/O でブロックしない。

---

## 5. サーバ設計

### 5.1 待受と接続

- Winsock 2.2。IPv4 を既定、IPv6 は設定で有効化。
- 既定待受: `0.0.0.0:7000`（LAN 利用を想定。ファイアウォールで NIC/サブネットを制限する運用を推奨）。
- `SO_REUSEADDR`、`TCP_NODELAY`、`SO_KEEPALIVE` を設定。
- ハンドシェイクに 5 秒のタイムアウト。超過で切断。
- 同時セッション数に上限（既定 8）。超過時は `ERROR(SESSION_LIMIT)` を返して切断。
- 1 接続 = 1 スレッド群。スレッド生成失敗時はセッションを破棄。

### 5.2 ConPTY セッション

Ecode の `Application/Terminal/include/ConPtySession.h` を基にする。サーバ用に以下を追加/変更する。

1. **ConPTY API ローダの堅牢化**
   - 探索順:
     1. 実行ファイル隣の `conpty.dll` の `ConptyCreatePseudoConsole` / `ConptyResizePseudoConsole` / `ConptyClosePseudoConsole`
     2. 同 `conpty.dll` の `CreatePseudoConsole` 系
     3. `kernel32.dll` の `CreatePseudoConsole` 系
   - どれも無い場合は起動時にエラーをログし、接続時に `ERROR(CONPTY_UNAVAILABLE)` を返す。
2. **出力の一時停止 / 再開**
   - `SetOutputPaused(bool)` を追加。
   - `IoLoop` は読み取りを再アームする前に paused を確認する。paused 中も**書き込みキューは処理する**。
   - 再開時は `ioWakeEvent_` をシグナルしてループを起こす。
3. **優先入力**
   - 既存の `WriteFront()` をそのまま使う。端末応答 (DSR/CPR/DA) を通常キーより先に PTY へ流す。
4. **リサイズ**
   - `Resize(cols, rows)` は `ResizePseudoConsole` を呼ぶ。最小 2x1、最大 1000x1000 にクランプ。
5. **終了順序**
   - 入力パイプを閉じる → 子プロセスの終了を待つ (既定 1.5 秒) → 未終了なら `TerminateProcess`
   - `ClosePseudoConsole` → 出力パイプ閉鎖 → スレッド join → ハンドル解放
   - Ecode 実装の順序を踏襲しつつ、**ソケット由来の終了と PTY 由来の終了を同じ状態機械で扱う**。

### 5.3 シェル起動

- シェル解決 (セッションごとに独立して行う):
  - `cmd`: `%COMSPEC%` または `%SystemRoot%\System32\cmd.exe`
  - `powershell`: `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`
  - `bash`: `PATH` 上の `bash.exe` → `C:\Program Files\Git\bin\bash.exe` →
    `C:\Program Files\Git\usr\bin\bash.exe` → `%LOCALAPPDATA%\Programs\Git\bin\bash.exe`
- `bash` は `--login -i` を付ける。`TERM=xterm-256color`、`COLORTERM=truecolor`、
  `CLICOLOR_FORCE=1`、`FORCE_COLOR=3` を環境ブロックに設定する。
- 作業ディレクトリはサーバ設定 (`cwd`)。既定はサーバ起動ユーザーのプロファイル。
- `CreateProcessW` に `EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT` と
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` を渡す。
- **クライアントから任意の実行ファイルを指定させない。** サーバ設定の許可リスト
  (`allowed_shells`) に一致する名前だけを受け付ける。パス指定を許可するのは
  明示的に `allow_arbitrary_shell = true` とした場合のみ。

### 5.4 セッション状態機械

```
IDLE
  └─ HELLO 受信 ──> HANDSHAKING
        ├─ 認証/版/シェル検証 NG ──> ERROR → CLOSING
        └─ OK ──> STARTING (ConPTY 起動)
              ├─ 失敗 ──> ERROR → CLOSING
              └─ 成功 ──> RUNNING
                    ├─ CLOSE / 切断 ──> DRAINING
                    ├─ 子プロセス終了 ──> DRAINING
                    └─ 内部エラー ──> ERROR → CLOSING
DRAINING (出力キューを掃き出す / 最大 1 秒)
  └─ EXIT 送信 ──> CLOSING
CLOSING (スレッド join / ハンドル解放)
  └─ CLOSED
```

- どの状態でも、ソケット送信は `SocketWriter` スレッドに一本化し、フレームの途中混在を防ぐ。
- 終了時は「入力停止 → 出力ドレイン → EXIT → 切断」の順を守る。

### 5.5 入出力とバックプレッシャ

- **PTY → クライアント**
  1. ConPTY 読み取りスレッドが最大 32 KiB を読み、`OUTPUT` フレームとして送信キューへ積む。
  2. キューが高水位 (既定 768 KiB) を超えたら `ConPtySession::SetOutputPaused(true)`。
  3. SocketWriter が送信して低水位 (既定 256 KiB) を下回ったら再開。
  4. これにより、クライアントが遅い場合は TCP → 送信キュー → PTY パイプ → 子プロセスの順で自然に待たされる。
- **クライアント → PTY**
  - `INPUT` は受信スレッドから直接 `ConPtySession::Write()` / `WriteFront()` へ渡す。
  - 入力は出力バックプレッシャの影響を受けない（別スレッド・別キュー）。
- **フレームの原子性**
  - 1 フレームの送信は `SocketWriter` 内で `send_all` により完結させる。他スレッドから直接 `send()` しない。
  - 制御フレーム (`RESIZED` / `ERROR` / `EXIT`) は出力キューとは別の優先キューに入れ、
    必要なら OUTPUT より先に送る。ただし `EXIT` はドレイン後に送る。

### 5.6 設定

`WinConsoleServer.ini` (キー=値) または CLI。CLI が優先。

```ini
listen = 0.0.0.0:7000
shell = cmd
cwd = %USERPROFILE%
allowed_shells = cmd,powershell,bash
allow_arbitrary_shell = false
max_sessions = 8
token =
handshake_timeout_ms = 5000
idle_timeout_ms = 0
output_high_water = 786432
output_low_water = 262144
max_frame_bytes = 1048576
log_file = logs/server.log
log_level = info
```

CLI 例:

```
WinConsoleServer.exe --listen 0.0.0.0:7000 --shell bash --cwd C:\work --token secret
```

### 5.7 セキュリティ (v1 の割り切り)

- LAN 前提・平文。**暗号化なし**であることをログと起動メッセージに明示する。
- `token` を設定した場合、HELLO の TLV で照合する。空の場合は認証なし。
- 推奨運用:
  - 待受を特定 NIC/サブネットに限定し、Windows ファイアウォールで許可元を絞る。
  - 必ず `token` を設定する。
  - `allow_arbitrary_shell = false` を維持する。
- 入力検証:
  - `payload_len` の上限、TLV 長の上限、文字列長の上限をチェックする。
  - ハンドシェイク前に OUTPUT を送らない。
  - 未知のフレームタイプは無視してログする（将来拡張のため）。
- 将来拡張:
  - 機能ビット `TLS` を交渉し、HELLO 前に SChannel で TLS ハンドシェイクする。
  - または `STARTTLS` フレームを追加し、フレーム層はそのままにトランスポートだけ差し替える。
  - 認証は `AUTH` フレーム / TLV を追加してトークン → チャレンジレスポンス → Windows 認証へ拡張可能。

---

## 6. プロトコル仕様 (WCSP v1.0)

### 6.1 トランスポート

- TCP。1 接続 = 1 セッション。
- すべての固定長整数は **リトルエンディアン**。
- 文字列は UTF-8（NUL 終端なし、長さは TLV 長で表現）。
- 接続直後は `HELLO` / `HELLO_ACK` のみ。ハンドシェイク完了まで `OUTPUT` は送らない。

### 6.2 フレーム

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       payload_len (uint32)                     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    type (u8)  |   flags (u8)  |         stream_id (u16)        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       payload (payload_len bytes)              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- ヘッダは 8 バイト。
- `payload_len` はヘッダを含まない。既定上限 1 MiB。超過は `ERROR(FRAME_TOO_LARGE)` で切断。
- `type`: フレーム種別。
- `flags`:
  - `INPUT`: bit0 = PRIORITY（端末応答。PTY の先頭に挿入）
  - 将来: bit1 = COMPRESSED、bit2 = MORE など
- `stream_id`: v1 は常に 0。将来の多重化用に予約。
- 送信側は 1 フレームを連続した `send` で送り切る。受信側は部分受信・複数フレーム同時受信を前提にストリーム解析する。

### 6.3 フレーム種別

| 値 | 名前 | 方向 | 用途 |
|----|------|------|------|
| 0x01 | `HELLO` | C→S | 接続要求、版、機能、初期サイズ、シェル、トークン |
| 0x02 | `HELLO_ACK` | S→C | 受理、交渉結果、実際のサイズ、セッション ID |
| 0x05 | `AUTH` | C→S | 予約（将来のチャレンジレスポンス） |
| 0x06 | `AUTH_RESULT` | S→C | 予約 |
| 0x10 | `INPUT` | C→S | VT 入力バイト列 |
| 0x11 | `RESIZE` | C→S | 画面サイズ変更要求 (cols, rows) |
| 0x12 | `RESIZED` | S→C | 適用結果 (cols, rows, reason) |
| 0x13 | `CLOSE` | C→S | 正常終了要求 |
| 0x14 | `PING` | C→S | キープアライブ |
| 0x15 | `PONG` | S→C | 応答 |
| 0x16 | `WINDOW_UPDATE` | C→S | 予約（クレジット制フロー制御） |
| 0x20 | `OUTPUT` | S→C | ConPTY の VT/UTF-8 バイト列 |
| 0x22 | `EXIT` | S→C | 子プロセス終了コードと理由 |
| 0x23 | `ERROR` | S→C | エラーコードとメッセージ |
| 0x24 | `TITLE` | S→C | 予約（サーバ側でタイトルを送る場合） |
| 0x7F | `NOOP` | both | 将来の拡張・テスト用 |

未知のタイプは受信側で読み捨てる（ただし `payload_len` は守る）。

### 6.4 TLV

`HELLO` / `HELLO_ACK` のペイロードは TLV の連結。

```
type (u16) | length (u32) | value (length bytes)
```

- 不明な TLV は無視する。
- 必須 TLV が無い場合は `ERROR(INVALID_HELLO)`。

#### HELLO

| TLV | 型 | 内容 |
|-----|----|------|
| 0x0000 | MAGIC | 4 バイト `"WCSP"`。必須 |
| 0x0001 | VERSION | `u16 major, u16 minor`。v1.0 = `1,0` |
| 0x0002 | CLIENT_NAME | UTF-8。例 `WinConsoleTerminal/0.2.0` |
| 0x0003 | FEATURES | `u32` 機能ビット |
| 0x0004 | SIZE | `u16 cols, u16 rows` (1..1000) |
| 0x0005 | SHELL | UTF-8。空ならサーバ既定。許可リスト照合 |
| 0x0006 | TOKEN | UTF-8。設定時のみ必須 |
| 0x0007 | ENV | UTF-8 `KEY=VALUE`。複数可。v1 は `TERM` 等の安全なキーのみ受理 |
| 0x0008 | CLIENT_ID | UTF-8。ログ用 |

#### HELLO_ACK

| TLV | 型 | 内容 |
|-----|----|------|
| 0x0001 | VERSION | サーバの版 |
| 0x0002 | SERVER_NAME | UTF-8 |
| 0x0003 | FEATURES | 交渉後の機能ビット（AND） |
| 0x0004 | SIZE | 実際に採用した `cols, rows` |
| 0x0005 | SESSION_ID | `u32` ログ相関用 |
| 0x0006 | SHELL | 実際に起動するシェルの表示名 |
| 0x0007 | ENCODING | 固定 `UTF-8`（将来用） |

### 6.5 機能ビット

| bit | 名前 | 意味 |
|-----|------|------|
| 0 | `RESIZE` | `RESIZE` / `RESIZED` をサポート |
| 1 | `PRIORITY_INPUT` | `INPUT.flags.PRIORITY` をサポート |
| 2 | `FLOW_CREDIT` | 将来のクレジット制フロー制御 |
| 3 | `COMPRESSION` | 将来の圧縮フレーム |
| 4 | `TLS` | 将来の TLS |
| 5 | `IMAGE_PASSTHROUGH` | OSC 1337 / sixel をそのまま転送 |
| 6 | `CLIPBOARD_OSC52` | OSC 52 をクライアントで処理 |
| 7 | `EXIT_CODE` | `EXIT` に終了コードを含める |

v1 のクライアント/サーバは `RESIZE | PRIORITY_INPUT | IMAGE_PASSTHROUGH | CLIPBOARD_OSC52 | EXIT_CODE` を広告する。

### 6.6 各フレームのペイロード

#### INPUT (0x10)

- ペイロード: VT 入力バイト列そのもの。
- `flags` bit0 = 1 の場合、サーバは `WriteFront()` を使う。
- 例: `a` = `0x61`、Enter = `0x0d`、Ctrl+C = `0x03`、矢印上 = `1b 5b 41`。

#### RESIZE (0x11)

```
u16 cols | u16 rows
```

#### RESIZED (0x12)

```
u16 cols | u16 rows | u16 reason
reason: 0 = applied, 1 = clamped
```

#### OUTPUT (0x20)

- ペイロード: ConPTY が生成したバイト列。**一切変換しない。**
- サーバは読み取ったチャンクをそのまま 1 フレームにする（既定 32 KiB 以下）。
- 圧縮は v1 では行わない。

#### EXIT (0x22)

```
u16 reason | u16 reserved | i32 exit_code
reason: 0 = child exited, 1 = server shutdown, 2 = client close, 3 = killed
```

#### ERROR (0x23)

```
u16 code | u16 reserved | UTF-8 message
```

エラーコード:

| コード | 名前 | 意味 |
|--------|------|------|
| 0x0001 | `INVALID_FRAME` | フレーム不正 |
| 0x0002 | `FRAME_TOO_LARGE` | 長さ上限超過 |
| 0x0101 | `VERSION_MISMATCH` | メジャー版不一致 |
| 0x0102 | `AUTH_REQUIRED` | トークン必須 |
| 0x0103 | `AUTH_FAILED` | トークン不一致 |
| 0x0104 | `SHELL_NOT_ALLOWED` | シェルが許可外 |
| 0x0105 | `SESSION_LIMIT` | 同時接続上限 |
| 0x0201 | `SHELL_START_FAILED` | 子プロセス起動失敗 |
| 0x0202 | `CONPTY_UNAVAILABLE` | ConPTY 不在 |
| 0x0301 | `INTERNAL` | 内部エラー |

#### PING / PONG (0x14 / 0x15)

```
u64 nonce
```

- クライアントは 15 秒ごとに送信。30 秒応答なしで切断扱い。
- `idle_timeout_ms` が 0 の場合はアプリケーションの無入力では切らない。

#### CLOSE (0x13)

```
u16 reason
```

クライアントからの正常終了要求。サーバは入力停止 → 出力ドレイン → `EXIT` の順で閉じる。

### 6.7 シーケンス例

```
C → S  HELLO        version=1.0, features, size=120x40, shell="bash", token
S → C  HELLO_ACK    version=1.0, features, size=120x40, session=42, shell="git-bash"
S → C  OUTPUT       "\x1b]0;...\x07" / プロンプト等
C → S  INPUT        "ls\r"
S → C  OUTPUT       "file1  file2\r\n..."
C → S  RESIZE       160x50
S → C  RESIZED      160x50, reason=0
...
C → S  CLOSE        0
S → C  OUTPUT       (残り)
S → C  EXIT         reason=2, code=0
```

### 6.8 拡張方針

- **TLS**: 機能ビットで交渉し、HELLO 前に TLS ハンドシェイクを挟む。フレーム層は変更しない。
- **認証**: `AUTH` / `AUTH_RESULT` を追加。トークン → チャレンジレスポンス → Windows 認証へ段階的に拡張。
- **圧縮**: `OUTPUT.flags` の COMPRESSED ビットと機能ビットで交渉。ペイロード先頭にアルゴリズム ID を置く。
- **多重化**: `stream_id` を使い、1 TCP 接続で複数セッションを扱う。`HELLO` に `OPEN` を追加する想定。
- **フロー制御**: `WINDOW_UPDATE` によるクレジット制。現状の TCP + バックプレッシャで不足する場合に導入。
- **WebSocket / SSH トランスポート**: フレーム層をそのまま使い、下位トランスポートだけ差し替える。

---

## 7. クライアント設計

### 7.1 UI (Native Win32)

UI は **Win32 API のみ**で構成する。メインウィンドウのクライアント領域に
`TerminalView`（`CreateWindowExW` で生成する子ウィンドウ）を全面配置する。

- ウィンドウクラス: `WinConsoleTerminalMain`（メイン） / `EcodeTerminalView`（既存）
- メインウィンドウ: `WS_OVERLAPPEDWINDOW`、メニュー、ステータスバー、リサイズ可能
- 子ウィンドウ: `WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS`
- メッセージ処理:
  - `WM_CREATE`: `TerminalView` 生成、`RemoteSession` 初期化、ステータスバー生成
  - `WM_SIZE`: `TerminalView::MoveAndResize()` → セルサイズ再計算 → `RESIZE`
  - `WM_SETFOCUS`: フォーカスを `TerminalView` へ
  - `WM_COMMAND`: メニュー / アクセラレータ
  - `WM_DPICHANGED`: 推奨 RECT 適用 + フォント再作成 + `RESIZE`
  - `WM_CLOSE`: セッションへ `CLOSE` 送信 → 受信スレッド join → 終了
  - `WM_TERMINAL_OUTPUT_WAKE`: 受信キューをドレインして `FeedOutput()`
  - `WM_TERMINAL_DISCONNECTED` / `WM_TERMINAL_EXIT`: 状態表示と入力無効化
- 接続ダイアログ:
  - `resource.rc` の `DIALOGEX` + `DialogBoxParamW`。
  - 項目: ホスト、ポート、シェル (`cmd` / `powershell` / `bash`)、トークン、初期 cols/rows、フォント、テーマ。
  - `WM_INITDIALOG` で前回設定をロード、OK 時に検証して保存。
  - 入力検証はダイアログ内で完結させ、接続失敗はメッセージボックスではなくステータスバー + ログに出す。
- ステータスバー:
  - `CreateWindowExW(STATUSCLASSNAMEW, ...)`。パーツ: 接続状態 / host:port / shell / cols x rows / RTT / 受信バイト。
- メニュー / アクセラレータ:
  - `LoadMenuW` + `CreateAcceleratorTableW`。接続 / 切断 / 再接続、コピー / 貼り付け、
    フォントサイズ、スクロールバック、接続情報、バージョン情報。
- 終了時:
  - `EXIT` 受信後は最終画面を保持し、「プロセスは終了しました (code=N)」をステータスバーに表示。
  - 異常切断時は「切断されました」と表示し、再接続メニューを有効化。
- DPI:
  - マニフェストで Per-Monitor V2。`WM_DPICHANGED` で `TerminalView` のフォントとセルサイズを更新し、`RESIZE` を送る。
- タイトル:
  - 既定は `WinConsoleTerminal - host:port`。OSC 0/2 を受けたら `SetWindowTextW` でリモート側タイトルを反映。
- リソース:
  - `resource.rc` にメニュー、ダイアログ、アイコン、バージョン情報、マニフェストを格納する。
  - 文字列は `LoadStringW` で取得し、将来のローカライズに備える。

ダイアログはモーダル (`DialogBoxParamW`) を基本とするが、接続中の切断確認などは
`MessageBoxW` を使わずステータスバー表示を優先し、作業を中断させない。

### 7.2 Ecode 部品の再利用

再利用するファイル (`D:/wsgit/Ecode/Application/Terminal`):

| ファイル | 用途 |
|----------|------|
| `include/TerminalCell.h` | セル属性・パレット |
| `include/TerminalBuffer.h`, `src/TerminalBuffer.cpp` | 画面グリッド・スクロールバック・選択 |
| `include/TerminalEmulator.h`, `src/TerminalEmulator.cpp` | VT/ANSI/OSC 解析 |
| `include/TerminalView.h`, `src/TerminalView.cpp` | Direct2D/DirectWrite 描画、キー入力、IME、マウス、画像 |
| `include/ImageManager.h`, `src/ImageManager.cpp` | OSC 1337 / sixel 画像 |
| `libsixel` (`third_party/libsixel/src`) | sixel デコード |

取り込み方法:

- `third_party/ecode_terminal/` にコピーし、元ファイルとの差分を最小化する。
- 変更点には `// WCS: ...` コメントを付け、Ecode 側へ還元できる形にする。
- Ecode の `settings.ini` 依存は、クライアント用の設定アダプタで吸収する。

### 7.3 `ITerminalIo` 抽象化

現状の `TerminalView` は `ConPtySession session_` に直接依存している
(`TerminalView.cpp` の `session_.WriteFront` / `session_.Start` / `session_.Resize` など)。
これを以下のインターフェースに置き換える。

```cpp
// client/ITerminalIo.h
struct TerminalStartOptions {
    std::wstring shell;                 // ローカル: 実行ファイル / リモート: 要求シェル名
    int cols = 80;
    int rows = 24;
    UINT codePage = CP_UTF8;
    std::vector<std::pair<std::wstring, std::wstring>> env;
};

class ITerminalIo {
public:
    using OutputFn = std::function<void(const char* data, size_t len)>;
    virtual ~ITerminalIo() = default;

    virtual void SetOutputSink(OutputFn fn) = 0;
    virtual bool Start(const TerminalStartOptions& opt, std::wstring* error) = 0;

    virtual void Write(const void* data, size_t len) = 0;      // 通常入力
    virtual void WriteFront(const void* data, size_t len) = 0; // 端末応答 (優先)
    virtual void Resize(int cols, int rows) = 0;

    virtual bool IsRunning() const = 0;
    virtual void Close() = 0;
};
```

実装:

| クラス | 用途 |
|--------|------|
| `LocalSession` | 既存 `ConPtySession` のラッパ。Ecode 内のローカルターミナル用 |
| `RemoteSession` | WinConsoleServer へ TCP 接続。今回のクライアント用 |

`TerminalView` の変更点:

```cpp
// before
ConPtySession session_;
// after
std::unique_ptr<ITerminalIo> io_;
```

| 現在の呼び出し | 変更後 |
|----------------|--------|
| `session_.SetOutputCallback(...)` | `io_->SetOutputSink(...)` |
| `session_.Start(shell, cols, rows, env)` | `io_->Start(options, &err)` |
| `session_.Write(...)` | `io_->Write(...)` |
| `session_.WriteFront(...)` | `io_->WriteFront(...)` |
| `session_.Resize(cols, rows)` | `io_->Resize(cols, rows)` |
| `session_.Close()` | `io_->Close()` |
| `session_.IsRunning()` | `io_->IsRunning()` |

出力経路:

- 現状は `ConPtySession` のコールバックから `WM_TERMINAL_OUTPUT` をチャンクごとに `PostMessage` している。
- リモートでは高負荷時にメッセージキューが溢れ得るため、次の方式に変更する。
  1. `ITerminalIo` の出力シンクは、バイト列をコピーして `std::deque` に積む。
  2. 未通知フラグを atomic で立て、未通知なら `WM_TERMINAL_OUTPUT_WAKE` を 1 回だけ `PostMessage`。
  3. UI スレッドはウェイクを受けたらキューを全部取り出し、`TerminalView::FeedOutput(chunk, len)` に渡す。
  4. キューのバイト数が高水位を超えたら受信スレッド/PTY 読み取りを一時停止する。
- `FeedOutput()` は既存の `OnTerminalOutput()` の UTF-8 部分シーケンス処理をそのまま利用する。

これにより、**出力バイト列を 1 バイトも落とさない**ことが保証できる。

### 7.4 `RemoteSession`

```cpp
class RemoteSession : public ITerminalIo {
public:
    explicit RemoteSession(RemoteConfig cfg);
    ~RemoteSession() override;

    void SetOutputSink(OutputFn fn) override;
    bool Start(const TerminalStartOptions& opt, std::wstring* error) override;

    void Write(const void* data, size_t len) override;
    void WriteFront(const void* data, size_t len) override;
    void Resize(int cols, int rows) override;

    bool IsRunning() const override;
    void Close() override;

    // UI への通知
    std::function<void(const ExitInfo&)> onExit;
    std::function<void(const std::wstring&)> onError;
};
```

- **接続**: Winsock、`TCP_NODELAY`、`SO_KEEPALIVE`。接続タイムアウト 5 秒。
- **ハンドシェイク**: `HELLO` を送り、`HELLO_ACK` を待つ。タイムアウト 5 秒。
  受信した実サイズで `TerminalBuffer` を補正する。
- **Receiver スレッド**:
  - フレームを解析。`OUTPUT` は受信キューへ。`RESIZED` は UI へ通知。`EXIT` は `onExit`。`ERROR` は `onError`。
  - 部分受信を保持するストリームパーサを使う。
  - 受信キューが高水位を超えたら `recv` を止める（TCP バックプレッシャ）。
- **Sender スレッド**:
  - 通常入力キューと優先入力キューを持つ。優先キューを先に送る。
  - 1 フレームずつ `send_all`。送信失敗でセッション終了。
  - `RESIZE` は最新値のみ保持し、33 ms 間隔で間引く（trailing edge は必ず送る）。
- **切断**:
  - `recv` 0 / エラー / PONG タイムアウトで `onError`。
  - UI スレッドに `WM_TERMINAL_DISCONNECTED` を送り、入力を無効化。
- **終了**:
  - `EXIT` 受信後、受信スレッドを join。UI は最終画面を保持して終了コードを表示。

### 7.5 入力の扱い

- `TerminalView` の既存 `EncodeKey()` が生成する VT シーケンスをそのまま `RemoteSession::Write()` に渡す。
- 端末応答 (`TerminalEmulator` の `onResponse_`) は `WriteFront()` に渡し、`INPUT.flags.PRIORITY` を付ける。
- 貼り付けは既存の bracketed paste 判定をそのまま利用する。
- IME の確定文字列も既存処理のまま UTF-8 で送る。
- マウス・フォーカス・代替画面は、リモート側アプリが出力したモード設定を
  `TerminalEmulator` が解釈するため、追加実装なしでローカルと同じように動作する。

### 7.6 リサイズ

1. ウィンドウの `WM_SIZE` / DPI 変更 / フォント変更でセルサイズを再計算する。
2. `TerminalView::OnSize()` が `cols` / `rows` を算出し、`buffer_.resize()` と `io_->Resize(cols, rows)` を呼ぶ。
3. `RemoteSession::Resize()` は最新値を保持し、33 ms デバウンスで `RESIZE` を送る。
4. サーバは `ResizePseudoConsole()` を呼び、`RESIZED(applied|clamped)` を返す。
5. クランプされた場合、クライアントは `buffer_.resize(ack)` で補正する。

注意:

- 初期サイズは `HELLO` に含める。ConPTY はそのサイズで起動するため、起動直後の 80x25 表示を避けられる。
- リモート側の `stty size` / `tput cols` / PowerShell の `RawUI.WindowSize` が一致することをテストする。

### 7.7 クライアント設定

`WinConsoleTerminal.ini`:

```ini
[connection]
host = 192.168.1.10
port = 7000
shell = bash
token =
auto_reconnect = false

[terminal]
font = Cascadia Mono
font_size = 11
scrollback = 10000
theme = dark
allow_osc52_clipboard = true
allow_osc1337_download = false
```

---

## 8. データフロー

### 8.1 入力 (キー → シェル)

```
UI thread: WM_KEYDOWN / WM_CHAR / IME / paste
  → TerminalView::EncodeKey() / UTF-8 変換
  → RemoteSession::Write() / WriteFront()
  → Sender thread キュー
  → INPUT フレーム (優先なら flags.PRIORITY)
  → TCP
  → Server FrameReader
  → ConPtySession::Write() / WriteFront()
  → ConPTY input pipe
  → bash / cmd / powershell
```

### 8.2 出力 (シェル → 画面)

```
bash / cmd / powershell
  → ConPTY output pipe
  → Server ConPTY output thread
  → 送信キュー (高水位で PTY 読み取りを停止)
  → SocketWriter
  → OUTPUT フレーム (バイト列そのまま)
  → TCP
  → Client Receiver thread
  → 受信キュー + WM_TERMINAL_OUTPUT_WAKE
  → UI thread: TerminalView::FeedOutput()
  → UTF-8 デコード + TerminalEmulator::process()
  → TerminalBuffer
  → Direct2D / DirectWrite 描画
```

### 8.3 リサイズ

```
WM_SIZE (client)
  → TerminalView::OnSize() → cols/rows 算出
  → buffer_.resize()
  → RemoteSession::Resize() (デバウンス)
  → RESIZE フレーム
  → Server: ResizePseudoConsole()
  → RESIZED フレーム
  → Client: 必要なら buffer 補正 / ステータス更新
```

### 8.4 終了

```
子プロセス終了
  → ConPTY 出力 EOF
  → Server: 出力ドレイン
  → EXIT(reason=child, code=N)
  → Client: 最終画面保持 + ステータス表示
  → 接続終了
```

---

## 9. 「bash からローカル端末に見える」ための設計

- ConPTY は子プロセスに対して**本物のコンソール**を提供する。`CreatePseudoConsole` +
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` で起動したプロセスは、
  `GetConsoleMode()` / `GetConsoleScreenBufferInfo()` / `isatty()` が成功する。
- パイプを直接 stdin/stdout に渡す方式では `isatty()` が偽になり、
  bash は非対話モードになってプロンプト・ジョブ制御・色が出ない。**必ず ConPTY を使う。**
- `bash` 起動時は `--login -i` を付け、`TERM=xterm-256color` を設定する。
- `winpty` / `mintty` を経由しない。Git Bash の `bash.exe` を直接 ConPTY に接続する。
- 入力は xterm 互換の VT シーケンス。`TerminalView::EncodeKey()` の出力をそのまま使う。
- リサイズは `ResizePseudoConsole()` により子プロセスへ伝わる。
  MSYS/Git Bash は `SIGWINCH` 相当を receiving し、`stty size` が新しい値を返す。
- Ctrl+C は `0x03` を INPUT として送る。ConPTY がコンソール入力に変換し、
  子プロセスへ Ctrl+C イベントとして伝える。`GenerateConsoleCtrlEvent()` は使わない。
- UTF-8 をそのまま往復させる。日本語・絵文字・全角文字はクライアントの
  `TerminalBuffer` が表示幅を計算する。`setAmbiguousWide(codePage == 932)` の扱いを踏襲する。

### 9.1 検証コマンド例

```bash
[ -t 0 ] && echo "tty OK"
tty
stty size
tput cols; tput lines
printf '\033[31mred\033[0m\n'
```

```powershell
$Host.UI.RawUI.WindowSize
```

---

## 10. ディレクトリ構成とビルド

実際の構成は 16.2 を参照。主要ターゲットだけ示す。

```
WinConsoleServer/
  CMakeLists.txt
  scripts/build.ps1, build.bat
  installer/WinConsoleServer.iss
  doc/design.md
  src/
    common/   protocol, frame_reader, conpty_session
    server/   main, server, session, server_config, app.manifest
    client/   main, remote_session, local_session, iterminal_io,
              resource.h, client.rc, app.manifest,
              terminal/ (Ecode からコピーした include/src)
  third_party/
    conpty/    conpty.dll, OpenConsole.exe
    libsixel/  sixel.h, config.h, src/ (ソースからビルド)
  tests/      test_protocol.cpp, test_frame_reader.cpp
```

CMake ターゲット:

| ターゲット | 種別 | 内容 |
|------------|------|------|
| `wcs_protocol` | STATIC | フレーム/TLV/メッセージのエンコード・デコード |
| `wcs_conpty` | STATIC | ConPTY ラッパ (サーバ/ローカル共用) |
| `libsixel` | STATIC | `third_party/libsixel/src` をソースからビルド |
| `ecode_terminal` | STATIC | Ecode の TerminalView/Emulator/Buffer/ImageManager |
| `WinConsoleServer` | CONSOLE | `wmain`。ws2_32 と wcs_* をリンク |
| `WinConsoleTerminal` | WIN32 | `wWinMain`。Direct2D/DirectWrite + ecode_terminal |
| `test_protocol` | CONSOLE | プロトコル単体テスト |
| `test_frame_reader` | CONSOLE | 分割受信テスト |

ビルドの要点:

- CRT は `/MT` (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`) で静的リンクする。
  libsixel も同じ設定でソースからビルドするため、VC 再頒布パッケージは不要。
- `conpty.dll` と `OpenConsole.exe` を POST_BUILD で exe の隣にコピーする。
- `/utf-8`、`UNICODE`、`_UNICODE`、`ECODE_ENABLE_SIXEL` を定義する。
- クライアントは `WIN32` サブシステム (`wWinMain`)、サーバはコンソールサブシステム (`wmain`)。
- マニフェストで Per-Monitor V2 DPI、コモンコントロール v6、`asInvoker` を宣言する。
- x64 のみを v1 の対象とする。

ビルドコマンドとインストーラの詳細は 16.8 / 16.9 を参照。

---

## 11. テスト計画

### 11.1 プロトコル

- フレームのシリアライズ / デシリアライズのラウンドトリップ。
- 1 バイトずつ分割受信、複数フレーム同時受信、巨大フレーム拒否。
- TLV の未知タイプ無視、長さ不正の拒否。
- 不正な `payload_len` でクラッシュしないこと。
- ファズテスト (ランダムバイト列をフレームパーサに投入)。

### 11.2 サーバ

- `cmd` / `powershell` / `bash` の起動とプロンプト表示。
- `[ -t 0 ]`、`stty size`、`tput cols` が期待値になること。
- リサイズ後の `stty size` が一致すること。
- Ctrl+C で `ping` や `sleep` が中断されること。
- 子プロセス終了時に `EXIT` と終了コードが届くこと。
- クライアント切断時にプロセスが残留しないこと。
- 同時 8 セッションで相互に影響しないこと。
- 出力 10 MB の連続出力でバイト欠落がないこと（クライアント側でハッシュ比較）。

### 11.3 クライアント / 表示

- 日本語、絵文字、全角文字、結合文字の表示と折り返し。
- `vim` / `less` / `top` / `htop` などの代替画面アプリ。
- 256 色 / TrueColor / 太字 / 下線 / 反転。
- スクロールバック、選択、コピー、貼り付け、bracketed paste。
- マウスレポート、フォーカスレポート。
- IME 入力。
- OSC 8 ハイパーリンク、OSC 52、OSC 1337 画像、sixel。
- DPI 変更、フォントサイズ変更、ウィンドウ最大化。
- 通信断、サーバ再起動、PING タイムアウト。
- VT シーケンスのコーパスを入力し、画面バッファのスナップショットを比較。

### 11.4 回帰テスト (telnetd の反省)

- 出力チャンクが任意の位置で分割されても、最終的な画面が一致すること。
- エスケープシーケンスの途中で TCP セグメントが切れても復元できること。
- 出力バースト時に 1 バイトも欠落しないこと。
- Enter / Backspace / Ctrl+C / 矢印キーの入力が二重にならないこと。

---

## 12. 性能目標

| 項目 | 目標 |
|------|------|
| LAN でのキー入力往復 | 30 ms 以下 (P95) |
| 連続出力スループット | 10 MB/s 以上 |
| アイドル時 CPU | 1 セッションあたり 1% 未満 |
| メモリ | 1 セッションあたり 20 MB 以下（スクロールバック含む） |
| 接続確立 | 500 ms 以内 (LAN) |
| リサイズ反映 | 100 ms 以内 |

- `TCP_NODELAY` を有効化。
- 出力は読み取ったチャンクを即送信し、小さなフレームを過度に結合しない（遅延優先）。
- スクロールバックはクライアント側の `TerminalBuffer` 上限で制御する。

---

## 13. 制約・リスク

| リスク | 内容 | 対策 |
|--------|------|------|
| ConPTY の版差 | Win10 1809 以降で挙動が異なる。古い環境では `conpty.dll` が必要 | API ローダでフォールバック。起動時に版をログ |
| MSYS/Git Bash の相性 | `bash.exe` の版によっては ConPTY での `isatty()` や `SIGWINCH` が不安定 | 検証マトリクスを用意。既定は Git for Windows の `bin/bash.exe` |
| UTF-8 とコードページ | サーバとクライアントで解釈がずれると文字化け・折り返し崩れ | プロトコルは UTF-8 固定。クライアントで一元的にデコード |
| 全角文字の幅 | 曖昧幅文字の扱いがアプリごとに異なる | `setAmbiguousWide()` を設定で切替可能にする |
| Ecode 部品の結合度 | `TerminalView` が `ConPtySession` に直接依存 | `ITerminalIo` で分離。差分を最小化し Ecode へ還元 |
| OSC 52 / OSC 1337 | リモートのシェルがローカルクリップボードやファイルにアクセスできる | 設定で無効化可能。ダウンロードは確認ダイアログ必須 |
| 平文通信 | LAN 内でも盗聴・改ざん可能 | v1 の割り切り。トークン + ファイアウォール。TLS 拡張点を用意 |
| 1 接続 1 セッション | 複数シェルを 1 接続で扱えない | `stream_id` を予約。v2 で多重化 |
| 再接続 | 切断でシェルが終了する | v1 は割り切り。v2 でデタッチ / 再接続を検討 |
| 画像転送 | OSC 1337 の画像が大きいとメモリを圧迫 | サイズ上限・確認ダイアログ・無効化設定 |

---

## 14. マイルストーン

| フェーズ | 内容 | 完了条件 |
|----------|------|----------|
| M0 | プロトコル定義、CMake 雛形、Ecode 部品の取り込み | ビルドが通る |
| M1 | サーバ + 最小クライアント (テキストのみ) | `cmd` のプロンプトが表示され、入力できる |
| M2 | `ITerminalIo` 抽象化、`RemoteSession`、Ecode 描画 | 色・カーソル・スクロールが正しく表示される |
| M3 | リサイズ、終了コード、バックプレッシャ | `stty size` 一致、10 MB 出力で欠落なし |
| M4 | `bash` / `powershell`、IME、マウス、貼り付け | 主要 TUI が動作 |
| M5 | 画像 / リンク / クリップボード、設定 UI | 実運用可能 |
| M6 | テスト、ログ、エラー処理、ドキュメント | 回帰テスト合格 |

---

## 15. 未決事項

1. クライアントを複数ウィンドウにするか、タブ対応にするか（v1 は 1 ウィンドウ 1 接続）。
2. サーバを Windows サービスとして動かすか（Session 0 の ConPTY とユーザー環境の扱い）。
3. 認証方式をトークンにするか、Windows アカウント (Negotiate) にするか。
4. セッションのデタッチ / 再接続を v2 に入れるか。
5. 圧縮を v1 から入れるか（LAN では不要と判断し、保留）。
6. OSC 1337 のファイルダウンロードを既定で無効にするか（案: 無効）。
7. Ecode 側へ `ITerminalIo` 変更を還元するタイミング。

---

## 付録 A. telnetd からの具体的な教訓 (要約)

1. **生の VT ストリームにプロトコル変換を挟まない。** 変換は必ずバイト欠落や状態不整合を生む。
2. **長さで境界を明示する。** ストリームの途中で切れても復元できる必要がある。
3. **エスケープシーケンスは 1 バイトも落とさない。** 欠落は端末状態の恒久的な破壊につながる。
4. **制御情報とコンソールデータを混ぜない。** エラーや終了は専用フレームで送る。
5. **サイズは最初に確定し、変更は ACK する。** 後追いの NAWS 頼みにしない。
6. **文字コードと表示幅はクライアントで一元管理する。** サーバはバイト列に責任を持たない。
7. **スレッドの終了順序と所有権を明確にする。** 切断時の use-after-free は表示破損と誤認される。
8. **クライアントの能力を仮定しない。** 専用クライアントでも機能ビットで交渉する。

---

## 16. 実装ガイド (Free モデル向け)

この章は「設計を読んだだけで実装できる」ことを目的に、**ファイル・関数・状態遷移・ビルド手順・落とし穴**を実装レベルまで落としたものである。
第 1〜15 章が仕様、この章が作業手順書に相当する。

### 16.1 現在の実装状況

| 項目 | 実装ファイル | 状態 |
|------|--------------|------|
| プロトコル (フレーム/TLV/各メッセージ) | `src/common/protocol.h/.cpp` | 実装済み。単体テスト合格 |
| 分割受信パーサ | `src/common/frame_reader.h/.cpp` | 実装済み。単体テスト合格 |
| ConPTY ラッパ | `src/common/conpty_session.h/.cpp` | 実装済み (Ecode 版 + 出力一時停止) |
| サーバ | `src/server/main.cpp`, `server.cpp`, `session.cpp`, `server_config.cpp` | 実装済み。`cmd` / `bash` で疎通確認済み |
| クライアント | `src/client/main.cpp`, `remote_session.cpp`, `local_session.cpp` | 実装済み。サーバへ接続し ESTABLISHED まで確認 |
| Ecode ターミナル部品 | `src/client/terminal/...` | コピー + `ITerminalIo` 対応に改造済み |
| ビルド | `CMakeLists.txt`, `scripts/build.ps1` | 実装済み。Release ビルド成功 |
| インストーラ | `installer/WinConsoleServer.iss` | 実装済み。ISCC コンパイル成功 |

未実装 / これから強化する項目:

- クライアント受信側のバックプレッシャ (現在はキューに積むだけ)
- サーバ設定ファイル (現在は CLI のみ)
- PING タイムアウトによる切断検知
- `powershell` の実機確認
- セッション上限の自動テスト
- ファイルログ、再接続、TLS、圧縮、多重化

### 16.2 実際のディレクトリ構成

```
WinConsoleServer/
  CMakeLists.txt
  scripts/
    build.ps1
    build.bat
  installer/
    WinConsoleServer.iss
  doc/
    design.md
  src/
    common/
      protocol.h / protocol.cpp
      frame_reader.h / frame_reader.cpp
      conpty_session.h / conpty_session.cpp
    server/
      main.cpp
      server.h / server.cpp
      session.h / session.cpp
      server_config.h / server_config.cpp
      app.manifest
    client/
      main.cpp
      remote_session.h / remote_session.cpp
      local_session.h / local_session.cpp
      iterminal_io.h
      resource.h
      client.rc
      app.manifest
      terminal/
        include/ (TerminalCell.h, TerminalBuffer.h, TerminalEmulator.h,
                  TerminalView.h, ImageManager.h)
        src/     (TerminalBuffer.cpp, TerminalEmulator.cpp,
                  TerminalView.cpp, ImageManager.cpp)
  third_party/
    conpty/ (conpty.dll, OpenConsole.exe)
    libsixel/ (sixel.h, config.h, src/)
  tests/
    test_protocol.cpp
    test_frame_reader.cpp
  bin/Release/          # ビルド生成物 (git 管理外)
  dist/                 # インストーラ (git 管理外)
```

### 16.3 Free モデル向けの実装順序

各ステップの最後に必ず「ビルド + テスト + 1 つの疎通確認」を行う。一度に複数ステップを進めない。

| ステップ | 作業 | 完了条件 |
|----------|------|----------|
| 0 | `scripts\build.bat` でビルド | `bin\Release\*.exe` が生成される |
| 1 | `test_protocol` / `test_frame_reader` を通す | 2 つとも OK |
| 2 | サーバを起動し Python クライアントで HELLO | `HELLO_ACK` が返る |
| 3 | `echo` を送って OUTPUT を受ける | マーカー文字列が返る |
| 4 | `RESIZE` を送る | `RESIZED` が返り、値が一致 |
| 5 | クライアント GUI で接続 | プロンプトが表示され、入力できる |
| 6 | インストーラをビルド | `dist\WinConsoleServerSetup-0.2.0.exe` が生成される |
| 7 | 未実装項目を 1 つずつ追加 | 16.11 の受け入れ条件を満たす |

### 16.4 プロトコル実装の詳細

#### 16.4.1 フレームヘッダ

```
offset  size  field
0       4     payload_len  uint32 little-endian (ヘッダを含まない)
4       1     type         uint8
5       1     flags        uint8
6       2     stream_id    uint16 little-endian (v1 は 0)
8       N     payload
```

- 受信側は `payload_len > max_payload` を即エラーにする。既定 1 MiB。
- 受信側は 1 回の `recv` で複数フレームが来ることを前提にする。
- 送信側は 1 フレームを `send_all` で送り切る。**他スレッドから同じソケットへ直接 `send` しない。**

#### 16.4.2 TLV

```
offset  size  field
0       2     type    uint16
2       4     length  uint32
6       N     value
```

- `length` は value のバイト数。
- 未知 type は読み捨てる。
- 同じ type が複数ある場合、HELLO では**最初の 1 つ**を採用する（ENV のみ複数可）。
- `length` が残りバイトを超えたら `DecodeTlvs` は失敗する。

#### 16.4.3 実装する関数 (シグネチャ固定)

`src/common/protocol.h` に定義済み。**勝手に引数や戻り値を変えない。**

```cpp
std::vector<uint8_t> EncodeFrame(const Frame& frame);
bool DecodeHeader(const uint8_t* data, size_t len, uint32_t& payload_len,
                  FrameType& type, uint8_t& flags, uint16_t& stream_id);

std::vector<uint8_t> EncodeHello(const Hello& hello);
bool DecodeHello(const uint8_t* data, size_t len, Hello& out, std::string* error);
std::vector<uint8_t> EncodeHelloAck(const HelloAck& ack);
bool DecodeHelloAck(const uint8_t* data, size_t len, HelloAck& out,
                    std::string* error);

std::vector<uint8_t> EncodeResize(const ResizeInfo& info);
bool DecodeResize(const uint8_t* data, size_t len, ResizeInfo& out);
std::vector<uint8_t> EncodeResized(const ResizedInfo& info);
bool DecodeResized(const uint8_t* data, size_t len, ResizedInfo& out);
std::vector<uint8_t> EncodeExit(const ExitInfo& info);
bool DecodeExit(const uint8_t* data, size_t len, ExitInfo& out);
std::vector<uint8_t> EncodeError(const ErrorInfo& info);
bool DecodeError(const uint8_t* data, size_t len, ErrorInfo& out);
std::vector<uint8_t> EncodePing(uint64_t nonce);
bool DecodePing(const uint8_t* data, size_t len, uint64_t& nonce);
```

#### 16.4.4 EncodeHello の擬似コード

```
out = []
AppendTlv(out, 0x0000, "WCSP")
AppendTlv(out, 0x0001, u16(major) + u16(minor))
AppendTlv(out, 0x0002, utf8(name))
AppendTlv(out, 0x0003, u32(features))
AppendTlv(out, 0x0004, u16(cols) + u16(rows))
if shell != "": AppendTlv(out, 0x0005, utf8(shell))
if token != "": AppendTlv(out, 0x0006, utf8(token))
if client_id != "": AppendTlv(out, 0x0008, utf8(client_id))
for env in envs: AppendTlv(out, 0x0007, utf8(env))
return out
```

#### 16.4.5 FrameReader の擬似コード

```
Append(data):
  offset_ > 0 なら先頭を詰める
  buffer_ の末尾に data を追加

Next(out):
  available = buffer_.size() - offset_
  if available < 8: return NeedMore
  DecodeHeader(...)
  if payload_len > max: Clear(); return Error
  total = 8 + payload_len
  if available < total: return NeedMore
  out = frame
  offset_ += total
  if offset_ == buffer_.size(): buffer_.clear(); offset_ = 0
  else if offset_ > 64KiB かつ offset_*2 > buffer_.size(): 先頭を詰める
  return Frame
```

不変条件:

- `Next()` は 1 バイトも捨てない（エラー時を除く）。
- エラー時のみ `Clear()` する。
- 部分フレームは次の `Append()` まで保持する。

### 16.5 サーバ実装の詳細

#### 16.5.1 起動フロー

```
wmain:
  ParseServerArgs
  SetConsoleCtrlHandler
  Server::Start
    WSAStartup
    getaddrinfo / socket / SO_REUSEADDR / bind / listen
  Server::Run
    accept loop
      接続ごとに std::thread を detach
        Session(socket, config, id, allowed, session_count).Run()
```

#### 16.5.2 Session::Run の手順

```
1. allowed でなければ ERROR(SESSION_LIMIT) を送って終了
2. ReadFrame(timeout=handshake_timeout_ms)
3. type != HELLO なら ERROR(INVALID_HELLO)
4. DecodeHello。失敗なら ERROR(INVALID_HELLO)
5. major != 1 なら ERROR(VERSION_MISMATCH)
6. token 設定あり:
     hello.token 空 -> ERROR(AUTH_REQUIRED)
     不一致 -> ERROR(AUTH_FAILED)
7. ResolveShell。失敗 -> ERROR(SHELL_NOT_ALLOWED)
8. cols/rows を 2..1000 / 1..1000 に clamp
9. HELLO_ACK を送る
10. pty_.SetOutputCallback(...)
11. pty_.Start(shell.command_line, cols, rows)
      失敗 -> ERROR(SHELL_START_FAILED)
12. writer_ スレッド開始
13. SO_RCVTIMEO = 200ms
14. 受信ループ:
      recv
        >0: reader_.Append -> Next を繰り返す -> HandleFrame
        =0: ClientClose
        <0 かつ WSAETIMEDOUT: 子プロセス終了を確認
      子プロセス終了 or !pty_.IsRunning() -> ループ終了
15. 終了処理:
      stopping_ = true
      pty_.SetOutputPaused(false)
      最大 500ms 出力キューをドレイン
      pty_.Close()
      EXIT フレームを enqueue
      stop_after_drain_ = true
      writer_.join()
      shutdown / closesocket
```

#### 16.5.3 ConPTY 起動の必須条件

- `CreatePseudoConsole` の前に名前付きパイプを 2 本作る。
- `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` を属性リストに入れる。
- `CreateProcessW` の `bInheritHandles` は **FALSE**。
  - TRUE にするとサーバのコンソールを子が継承し、`cmd.exe` の出力がサーバ側コンソールに出る。
  - 実際にこの問題を踏んだので、変更しないこと。
- 環境変数 `TERM=xterm-256color` などを設定する。
- 子プロセス終了後は `ClosePseudoConsole` を必ず呼ぶ。

#### 16.5.4 バックプレッシャ

```
EnqueueFrame(OUTPUT):
  queue_bytes_ += payload.size()
  if queue_bytes_ >= output_high_water: pty_.SetOutputPaused(true)
  queue_.push_back(frame)

WriterLoop:
  frame を送信
  if frame.type == OUTPUT:
    queue_bytes_ -= payload.size()
    if queue_bytes_ <= output_low_water: pty_.SetOutputPaused(false)
```

- `SetOutputPaused(true)` は **読み取りの再アームを止めるだけ**。入力書き込みは継続する。
- `SetOutputPaused(false)` は `ioWakeEvent_` をシグナルして再アームさせる。
- これにより Ctrl+C が出力バーストで遅延しない。

#### 16.5.5 よくある失敗

| 症状 | 原因 | 対処 |
|------|------|------|
| 子プロセスの出力がサーバのコンソールに出る | `bInheritHandles=TRUE` | FALSE にする |
| 起動直後にセッションが閉じる | `GetProcessHandle` を見て即終了判定 / ConPTY 失敗 | ログを追加し、`IsRunning()` と exit code を確認 |
| 出力が途中で止まる | キュー満杯で pause したまま | low water で必ず resume する |
| 終了時にハング | writer が send でブロック | `shutdown(SD_BOTH)` してから join |
| `bash` が WSL になる | `SearchPathW` が System32 の bash を拾う | Git for Windows のパスを優先する |

### 16.6 クライアント実装の詳細

#### 16.6.1 ITerminalIo の契約

- `SetOutputSink` は `Start` より前に呼ぶ。
- `Start` は成功時のみ true。失敗時は `error` に理由を入れる。
- `Write` / `WriteFront` / `Resize` は UI スレッドから呼ばれる。**ブロックしてはいけない。**
- `Close` は何度呼ばれても安全。スレッドを join する。
- `IsRunning` はスレッドセーフ。

#### 16.6.2 TerminalView の改造点 (Ecode からの差分)

| 変更 | 内容 |
|------|------|
| include | `ConPtySession.h` → `../../iterminal_io.h` |
| メンバ | `ConPtySession session_` → `std::unique_ptr<ITerminalIo> io_` |
| 追加 API | `SetIo`, `FeedOutput`, `Columns`, `Rows` |
| 応答 | `session_.WriteFront` → `io_->WriteFront` (UTF-8 変換) |
| 入力 | `session_.Write` → `io_->Write` |
| サイズ | `session_.Resize` → `io_->Resize` |
| 出力 | コールバックで `EnqueueOutput` → `WM_TERMINAL_OUTPUT_WAKE` → UI で `OnTerminalOutput` |
| 終了 | `session_.Close` → `io_->Close` |

**VT パーサ (`TerminalEmulator.cpp`) とバッファ (`TerminalBuffer.cpp`) は改造しない。**
Ecode との差分を最小化し、将来還元できるようにする。

#### 16.6.3 出力キューの不変条件

- I/O スレッドは `std::vector<uint8_t>` にコピーして `std::deque` に積む。
- `PostMessage(WM_TERMINAL_OUTPUT_WAKE)` は**未通知のとき 1 回だけ**。
- UI スレッドはキューを丸ごと swap してから処理する。
- `outputWakePosted_` は swap 時に false に戻す。
- これにより、高負荷時もメッセージキューを溢れさせず、バイトを落とさない。

#### 16.6.4 RemoteSession の状態

```
Idle
  Start()
    WSAStartup
    Connect (non-blocking connect + select)
    Handshake (HELLO -> HELLO_ACK)
    running_ = true
    receiver_ / sender_ 開始
Running
  Write/WriteFront -> キュー
  Resize -> RESIZE キュー
  Receiver -> OUTPUT -> sink / RESIZED / EXIT / ERROR
  Close -> stopping_ = true, shutdown, join
Closed
```

- ハンドシェイク中に `HELLO_ACK` の後ろに OUTPUT が来ても、`FrameReader` が保持しているので失わない。
- `Close` は `stopping_` の値を問わず join する（受信スレッドが先に停止している場合がある）。
- `WSAStartup` は `Start` の最初、`WSACleanup` は `Close` の最後。

#### 16.6.5 UI メッセージ

| メッセージ | 用途 |
|------------|------|
| `WM_TERMINAL_OUTPUT_WAKE` | TerminalView が出力キューをドレイン |
| `WM_APP_ERROR` | RemoteSession のエラー。UI で `LastError()` を表示 |
| `WM_APP_EXIT` | 子プロセス終了。wParam に終了コード |
| `WM_APP_RESIZED` | サーバ適用後のサイズ。lParam に cols/rows |

- ワーカースレッドから UI を直接触らない。必ず `PostMessage` する。
- ダイアログは `DialogBoxParamW`。コントロール ID は `resource.h` に固定。

### 16.7 Ecode 部品の取り込み手順

1. `D:/wsgit/Ecode/Application/Terminal/include` から以下をコピー:
   - `TerminalCell.h`, `TerminalBuffer.h`, `TerminalEmulator.h`, `TerminalView.h`, `ImageManager.h`
2. `D:/wsgit/Ecode/Application/Terminal/src` から以下をコピー:
   - `TerminalBuffer.cpp`, `TerminalEmulator.cpp`, `TerminalView.cpp`, `ImageManager.cpp`
3. `ConPtySession.h/.cpp` は `src/common/` にコピーし、サーバとクライアントの両方から使う。
4. `ImageManager.cpp` の `#include "../../libsixel-windows/include/sixel.h"` を
   `#include <sixel.h>` に変更する。`libsixel` は `third_party/libsixel/src` の
   ソースから静的ライブラリとしてビルドする（`/MT` を揃えるため）。
5. `TerminalView` を 16.6.2 の表に従って改造する。
6. `ecode_terminal` 静的ライブラリとしてビルドする。
7. 改造箇所には `// WCS:` コメントを付ける（Ecode へ還元するため）。

#### 16.7.1 Ecode 側の更新を取り込む手順

Ecode の `Application/Terminal` が更新されたら、以下の順で取り込む。

1. 変更されたファイルを確認する。

```powershell
cd D:\wsgit\Ecode
git status --short -- Application/Terminal
git diff HEAD -- Application/Terminal
```

2. 次のファイルは**そのままコピー**する（WCS 側で改造していない）。
   - `include/TerminalCell.h`
   - `include/TerminalBuffer.h`, `src/TerminalBuffer.cpp`
   - `include/TerminalEmulator.h`, `src/TerminalEmulator.cpp`
   - `include/ImageManager.h`, `src/ImageManager.cpp`
   - `src/ImageManager.cpp` はコピー後に `#include <sixel.h>` へ変更する。

3. `TerminalView.h/.cpp` は**そのままコピーした後に WCS 改造を再適用**する。
   再適用する差分は次のとおり。
   - `ConPtySession.h` → `../../iterminal_io.h`
   - `session_` メンバ → `std::unique_ptr<ITerminalIo> io_`
   - `SetIo` / `StartSession(TerminalStartOptions)` / `FeedOutput` / `Columns` / `Rows`
   - 出力を `EnqueueOutput` → `WM_TERMINAL_OUTPUT_WAKE` → `DrainOutputQueue` に変更
   - `session_.Write` / `WriteFront` / `Resize` / `Close` を `io_->...` に置換
   - `WM_TERMINAL_OUTPUT_WAKE` の case を追加
   - 応答コールバックの `std::wstring` を UTF-8 に変換して `io_->WriteFront`

4. ビルドとテスト。

```powershell
.\scripts\build.bat
.\bin\Release\tests\test_protocol.exe
.\bin\Release\tests\test_frame_reader.exe
```

5. サーバ + クライアントで疎通確認（16.10）。

注意:

- Ecode 側の変更を `diff --strip-trailing-cr` で確認すると、WCS の差分だけを抽出できる。
- Ecode の VT パーサ (`TerminalEmulator.cpp`) とバッファ (`TerminalBuffer.cpp`) は改造しない。
- Ecode 側で `TerminalView` の描画・選択・点滅が変わった場合も、上記の I/O 置換だけを再適用する。

### 16.8 ビルド環境の構築

#### 16.8.1 必要なもの

| ツール | 確認済みバージョン | 用途 |
|--------|--------------------|------|
| Visual Studio 2022 Community | MSVC 19.44 | C++17 コンパイラ |
| Windows SDK | 10.0.26100 | Win32 / Direct2D |
| CMake | 3.31.6 | ビルド生成 |
| Inno Setup 6 | 6.x | インストーラ |
| Git for Windows | 任意 | `bash.exe` の供給元 |

Visual Studio インストーラで「C++ によるデスクトップ開発」と Windows SDK を入れる。

#### 16.8.2 コマンド

```powershell
cd D:\wsgit\WinConsoleServer

# 構成 + ビルド + テスト
.\scripts\build.bat

# インストーラまで
.\scripts\build.bat -Installer

# クリーンビルド
.\scripts\build.bat -Clean -Installer
```

手動で行う場合:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\WinConsoleServer.iss
```

#### 16.8.3 生成物

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

#### 16.8.4 トラブルシューティング

| エラー | 原因 | 対処 |
|--------|------|------|
| `LNK1104: ...exe を開けません` | 実行中のプロセスが exe をロック | `taskkill /IM WinConsoleServer.exe /F` して再ビルド |
| `conpty.dll` が見つからない | POST_BUILD コピー前 / 手動実行 | `bin\Release` に DLL があるか確認 |
| `winsock2.h` と `windows.h` の衝突 | include 順序 | `winsock2.h` → `ws2tcpip.h` → `windows.h` の順にする |
| 空の文字定数エラー | ソースに実際の NUL (0x00) が入った | `'\0'` をバックスラッシュ + `0` で書き直す |
| `/Zc:strictStrings` エラー | `L""` を `LPWSTR` に代入 | キャストするか `/permissive-` を外す（Ecode に合わせる） |
| サーバは起動するが子が見えない | `bInheritHandles=TRUE` | FALSE にする |

### 16.9 インストーラ (Inno Setup 6)

- スクリプト: `installer/WinConsoleServer.iss`
- 出力: `dist/WinConsoleServerSetup-0.2.0.exe`
- インストール先: `{autopf}\WinConsoleServer` (通常 `C:\Program Files\WinConsoleServer`)
- 同梱物:
  - `WinConsoleServer.exe`
  - `WinConsoleTerminal.exe`
  - `conpty.dll`
  - `OpenConsole.exe`
  - `doc\design.md`
- タスク:
  - デスクトップアイコン (任意)
  - Windows ファイアウォール TCP 7000 許可 (既定でチェック)
- アンインストール時にファイアウォール規則を削除する。
- サイレントインストール:

```powershell
.\dist\WinConsoleServerSetup-0.2.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

バージョンを上げる手順:

1. `installer/WinConsoleServer.iss` の `MyAppVersion` を変更
2. `src/client/main.cpp` の About 表記を変更
3. 再ビルド + ISCC

### 16.10 動作確認手順

#### 16.10.1 サーバ単体

```powershell
.\bin\Release\WinConsoleServer.exe --listen 127.0.0.1:7000 --shell cmd
```

別ターミナルで:

```powershell
.\bin\Release\WinConsoleTerminal.exe --host 127.0.0.1 --port 7000 --shell cmd --connect
```

確認項目:

- プロンプトが表示される
- 文字入力・Enter・Backspace が正しい
- ウィンドウをリサイズすると `stty size` / `mode con` が追従する
- Ctrl+C が効く
- 終了コードがステータスバーに出る

#### 16.10.2 bash

```powershell
.\bin\Release\WinConsoleServer.exe --listen 127.0.0.1:7000 --shell bash
```

クライアントから接続し、bash で:

```bash
[ -t 0 ] && echo TTY_OK
tty
stty size
tput cols; tput lines
printf '\033[31mRED\033[0m\n'
```

期待:

- `TTY_OK` が出る
- `stty size` がクライアントの cols/rows と一致する
- 色が表示される
- 日本語が文字化けしない

#### 16.10.3 プロトコル単体テスト

```powershell
.\bin\Release\tests\test_protocol.exe
.\bin\Release\tests\test_frame_reader.exe
```

両方 `OK` を出力すること。

#### 16.10.4 インストーラ

1. `dist\WinConsoleServerSetup-0.2.0.exe` を実行
2. インストール先に 4 つの exe/dll があること
3. スタートメニューから `WinConsoleTerminal` が起動すること
4. アンインストールでファイアウォール規則が消えること

### 16.11 次の実装タスク (優先順)

| 優先 | タスク | 変更ファイル | 受け入れ条件 |
|------|--------|--------------|--------------|
| 1 | クライアント受信バックプレッシャ | `iterminal_io.h`, `remote_session.cpp`, `terminal_view.cpp` | 受信キューが高水位を超えたら `recv` を止め、ドレイン後に再開する。10MB 出力で欠落しない |
| 2 | サーバ INI 設定 | `server_config.cpp` | `WinConsoleServer.ini` の `listen`, `shell`, `token`, `max_sessions` が CLI なしで効く |
| 3 | PING タイムアウト | `remote_session.cpp` | 30 秒 PONG なしで `onError` → 切断表示 |
| 4 | `powershell` 実機確認 | なし | プロンプト表示、リサイズ、Ctrl+C |
| 5 | セッション上限テスト | `session.cpp` | 9 接続目が `ERROR(SESSION_LIMIT)` を受ける |
| 6 | ファイルログ | `server.cpp`, `session.cpp` | `logs\server.log` に接続/切断/エラーが残る |
| 7 | 再接続 | `main.cpp` | 切断後に同じ設定で再接続できる |
| 8 | TLS | `remote_session.cpp`, `session.cpp` | 機能ビット `TLS` 交渉時のみ TLS。フレーム層は不変 |
| 9 | 圧縮 | `protocol.cpp` | `OUTPUT.flags` の COMPRESSED で zstd 等を後付け |
| 10 | Ecode へ還元 | `TerminalView` | `ITerminalIo` 変更を Ecode 側へ PR |

### 16.12 Free モデル向け実装ルール

1. **プロトコルの数値を発明しない。** `src/common/protocol.h` の定数と enum を使う。
2. **1 変更ごとにビルドする。** エラーが出たら、まず直近の変更を疑う。
3. **バイトを落とさない。** 出力経路に「切り捨て」「上限で break」「drop」を書かない。
4. **UI スレッドでブロックしない。** `recv` / `send` / `join` を UI スレッドで呼ばない。
5. **スレッドは必ず join する。** `detach` はサーバのセッションスレッドだけに限定する。
6. **UTF-8 と UTF-16 を混ぜない。** プロトコル/端末ストリームは UTF-8、Win32 API は UTF-16。
7. **`session_` を復活させない。** TerminalView は `io_` 経由に統一する。
8. **`bInheritHandles=FALSE` を維持する。** ConPTY の必須条件。
9. **迷ったら 16.10 の疎通確認に戻る。** 単体テスト → サーバ単体 → GUI の順。
10. **Ecode の VT パーサを書き換えない。** 表示の問題はまずプロトコル/入力を疑う。
