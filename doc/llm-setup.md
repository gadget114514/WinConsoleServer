# LLM 向けインストール・設定ガイド

この文書は、LLM エージェントが WinConsoleServer のインストール後に
**非対話で設定・検証**するための手順をまとめたものです。
人間向けの概要は `README.md` を参照してください。

## 0. 前提と安全条件

- OS: Windows 10 1809 (Build 17763) 以降 / x64
- PowerShell は管理者権限で実行する（サービス登録・ファイアウォール設定時）
- インストール先の既定値: `C:\Program Files\WinConsoleServer`
- 設定・ログの既定値: `C:\ProgramData\WinConsoleServer`
- WCSP は**平文 TCP** です。信頼できないネットワークへ公開しないでください。
- 外部公開する場合は `--listen 127.0.0.1:7000` と SSH ポート転送などを併用してください。
- トークンは秘密情報です。LLM はトークン値をログ・チャット・標準出力へ出さないでください。
- コマンドは原則として絶対パスで実行し、各手順後に `$LASTEXITCODE` を確認してください。

## 1. インストール結果の確認

```powershell
$InstallDir = Join-Path $env:ProgramFiles "WinConsoleServer"
$Server     = Join-Path $InstallDir "WinConsoleServer.exe"
$Terminal   = Join-Path $InstallDir "WinConsoleTerminal.exe"
$Wct        = Join-Path $InstallDir "wct.exe"
$WcsCli     = Join-Path $InstallDir "wcs-cli.exe"
$DataDir    = Join-Path $env:ProgramData "WinConsoleServer"
$TokenFile  = Join-Path $DataDir "token.txt"
$ServerLog  = Join-Path $DataDir "server.log"

@($Server, $Terminal, $Wct, $WcsCli) | ForEach-Object {
    if (-not (Test-Path -LiteralPath $_)) {
        throw "missing file: $_"
    }
}
```

インストーラで「Windows サービスとして登録」を選択した場合、サービス名は
`WinConsoleServer`、待受は `0.0.0.0:7000`、シェルは `cmd` です。

```powershell
sc.exe query WinConsoleServer
```

## 2. 設定値の決定

LLM は次の値を決めてからコマンドを組み立ててください。

| 設定 | 既定値 | 指定方法 |
|------|--------|----------|
| 待受アドレス | `0.0.0.0:7000` | `--listen host:port` |
| 既定シェル | `cmd` | `--shell cmd\|powershell\|bash` |
| 作業ディレクトリ | ユーザープロファイル | `--cwd path` |
| 同時セッション上限 | `8` | `--max-sessions N` |
| リプレイ保持量 | `32 MiB` | `--history-bytes N` (64 KiB〜64 MiB) |
| トークン | サービス時は自動生成 | `--token` / `--token-file` / `WCS_TOKEN` |
| ログ | サービス時は `server.log` | `--log-file path` |
| サービス名 | `WinConsoleServer` | `--service-name name` |

トークンの優先順位:

1. `--token secret`
2. `--token-file path`
3. 環境変数 `WCS_TOKEN`
4. サービスインストール時の自動生成 (`%ProgramData%\WinConsoleServer\token.txt`)

コンソールモードでトークンを指定しない場合、認証は無効です。

## 3. コンソールモードで起動する

サービスを使わず、その場で起動して確認する場合:

```powershell
& $Server --listen 127.0.0.1:7000 --shell cmd
```

別のターミナルで確認:

```powershell
& $Wct --host 127.0.0.1 --port 7000 --list
```

`--list` が保持セッション一覧を返せば、サーバとトークン設定は有効です。

## 4. Windows サービスとして設定する

管理者 PowerShell で実行します。

```powershell
& $Server --install-service `
    --listen 0.0.0.0:7000 `
    --shell cmd `
    --history-bytes 33554432
if ($LASTEXITCODE -ne 0) { throw "service install failed: $LASTEXITCODE" }

sc.exe query WinConsoleServer
if ($LASTEXITCODE -ne 0) { throw "service is not registered" }
```

トークン未指定の場合、インストール時に `token.txt` が生成されます。
生成されたかだけを確認し、値は表示しないでください。

```powershell
if (-not (Test-Path -LiteralPath $TokenFile)) {
    throw "token file was not generated: $TokenFile"
}
```

サービスを後から開始・停止する場合:

```powershell
sc.exe start WinConsoleServer
sc.exe stop WinConsoleServer
```

サービスモードのログは既定で `$ServerLog` に追記されます。

## 5. トークンを設定・変更する

### 5.1 サービス用トークンファイルを変更する

```powershell
# 管理者 PowerShell
& $WcsCli set-token --server
```

既存トークンを確認せずに再生成する場合も同じコマンドです。
変更後はサービスを再起動してください。

```powershell
sc.exe stop WinConsoleServer
sc.exe start WinConsoleServer
```

### 5.2 クライアント用トークンを保存する

```powershell
& $WcsCli set-token
```

保存先は `%APPDATA%\WinConsoleServer\wcs-cli.ini` です。
`wct.exe` と `WinConsoleTerminal.exe` へは `--token` または `WCS_TOKEN` で渡せます。

### 5.3 任意ファイルへ書き込む

```powershell
& $WcsCli set-token --file C:\path\to\token.txt
```

## 6. ファイアウォールを設定する

インストーラでファイアウォール規則を選択した場合は不要です。
手動でサービスを構成した場合は、管理者 PowerShell で実行します。

```powershell
$RuleName = "WinConsoleServer TCP 7000"
$Existing = Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
if (-not $Existing) {
    New-NetFirewallRule -DisplayName $RuleName `
        -Direction Inbound -Protocol TCP -LocalPort 7000 -Action Allow | Out-Null
}
```

外部公開しない場合は、この規則を作らず `--listen 127.0.0.1:7000` を使用してください。

## 7. クライアント設定

### 7.1 WinConsoleTerminal.exe

設定ファイル: `%APPDATA%\WinConsoleTerminal.ini`

```ini
[connection]
host=127.0.0.1
port=7000
shell=cmd
token=<token>
```

`token` は平文で保存されます。共有端末では `WCS_TOKEN` の利用を検討してください。

### 7.2 wct.exe / wcs-cli.exe

- `wct.exe`: `--host`, `--port`, `--token`, `--token-file`, `--attach <id>`
- `wcs-cli.exe`: `list`, `delete`, `set-token`
- 共通の設定ファイル: `%APPDATA%\WinConsoleServer\wcs-cli.ini`
- 環境変数: `WCS_TOKEN`

例:

```powershell
$Token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
& $Wct --host 127.0.0.1 --port 7000 --token $Token --list
```

トークン値を画面へ表示しないでください。

## 8. 設定後の検証

```powershell
# 1. サービス状態
sc.exe query WinConsoleServer

# 2. ポート待受 (0.0.0.0:7000 または 127.0.0.1:7000)
Get-NetTCPConnection -LocalPort 7000 -State Listen -ErrorAction SilentlyContinue

# 3. トークンで一覧取得
$Token = (Get-Content -LiteralPath $TokenFile -Raw).Trim()
& $Wct --host 127.0.0.1 --port 7000 --token $Token --list
if ($LASTEXITCODE -ne 0) { throw "wct --list failed" }

# 4. GUI クライアント
Start-Process -FilePath $Terminal `
    -ArgumentList "--host 127.0.0.1 --port 7000 --shell cmd --connect"
```

## 9. LLM 実行用チェックリスト

LLM は上から順に実行し、失敗した手順で停止してください。

1. `$InstallDir` 配下に `WinConsoleServer.exe` / `WinConsoleTerminal.exe` / `wct.exe` / `wcs-cli.exe` があるか確認する。
2. 管理者権限が必要な操作（サービス登録、ファイアウォール）の前に権限を確認する。
3. 待受アドレスを決める。外部公開しない場合は `127.0.0.1:7000` を使う。
4. `--install-service` を実行する。失敗したら `$LASTEXITCODE` と `$ServerLog` を確認する。
5. `token.txt` の存在を確認する。値は表示しない。
6. 必要ならファイアウォール規則を追加する。
7. `wct --list` で接続確認する。
8. 失敗時は `sc.exe query`、`Get-NetTCPConnection`、`$ServerLog` の順に確認する。
9. 設定変更後はサービスを再起動する。
10. アンインストール時は `--uninstall-service` を実行し、必要ならファイアウォール規則を削除する。

## 10. アンインストール

```powershell
# 管理者 PowerShell
& $Server --uninstall-service

Remove-NetFirewallRule -DisplayName "WinConsoleServer TCP 7000" `
    -ErrorAction SilentlyContinue
```

アンインストーラを使う場合は、インストール時と同様に管理者権限で実行してください。

## 11. トラブルシューティング

| 症状 | 確認事項 | 対処 |
|------|----------|------|
| サービスが起動しない | `sc.exe query WinConsoleServer`、`$ServerLog` | `--token-file` のパス、`--listen` のポート競合を確認 |
| `wct --list` が認証エラー | `token.txt` と `--token` の値 | サービス再起動後に再試行 |
| 接続できない | `Get-NetTCPConnection -LocalPort 7000` | ファイアウォール、待受アドレス、ポート番号を確認 |
| `bash` が見つからない | Git for Windows の有無 | `--shell cmd` または `powershell` を使用 |
| ポート 7000 が使用中 | `Get-NetTCPConnection -LocalPort 7000` | `--listen host:7001` などへ変更し、クライアント側も合わせる |

サーバの全オプションは次で確認できます。

```powershell
& $Server --help
```
