# jmbraille - LaTeXソースからの点訳自動生成パッケージ & システム

![LaTeX](https://img.shields.io/badge/LaTeX-uplatex%20%7C%20xelatex%20%7C%20lualatex-blue)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-green)

`jmbraille` は、LaTeX で作成された文書ソースをコンパイルすることで、墨字（一般的な印刷文字）と点字（点字プリンタ・ピンディスプレイ用出力）の双方を同一ソースから自動生成するための LaTeX パッケージおよび連携アプリ群です。

特に日本の数学点字規格や、日本語（MeCab利用）・英語（Liblouis利用）のハイブリッド点訳、図表の立体コピー・プロッタプリンタ用データ出力に対応しています。

使い方の詳細は [jmbraille-manual-sumiji.pdf](./jmbraille-manual-sumiji.pdf) または [jmbraille-manual.nab](./jmbraille-manual.nab) をご覧ください。

---

## 🚀 主な機能と特徴

- **同一ソースからの墨字/点字出力:**
  - 通常のコンパイルで墨字出力。
  - パッケージオプション `braille` を付与することで点字出力。
- **日本語・英語・数式点訳:**
  - 形態素解析エンジン **MeCab** による日本語分かち書き・読み付与。
  - 英語点訳ライブラリ **Liblouis** による英語出力（UEB Grade 1/2 対応）。
  - 日本独自の数学点字規格に対応した数式変換。
- **多様な出力形式への対応:**
  - 点字データ（`.nab`, `.gnb`）
  - Unicode 8点点字テキスト（`.txt` - ピンディスプレイ用）
  - 立体コピー・プロッタプリンタ用図表データ（`.pdf`, `.edl`）

---

## 💻 動作環境

### 推奨環境
- **LaTeX システム:** 2015年以降の TeX システム（`uplatex`, `xelatex`, `lualatex` に対応）
- **依存プログラム:** （自分でコンパイルしない限りインストール不要）
  - [MeCab](https://taku910.github.io/mecab/) （日本語形態素解析）
  - [Liblouis](https://liblouis.io/) （英語点訳ライブラリ）
- **対応 OS:** Windows / macOS / Linux

---

## 🛠 最初の準備 (Setup)

1. **Windows / macOS の場合:**
   - 本リポジトリをダウンロード（または `git clone`）して解凍します。
   - `jmbraille/exec/` 内にあるバイナリに実行権限があるか確認し、必要に応じてパーミッションを変更してください。
2. **Linux の場合:**
   - `source/` 内にあるソースコードをご自身の環境でコンパイルしてください（要 MeCab, Liblouis）。
3. **TeX システムの確認:**
   - TeX システムが 2021 年以前の古いバージョンの場合、`\usepackage[file,...]{jmbraille}` のように `file` オプションを追加する必要がある場合があります（Windows 環境は自動的に `file` 利用）。

---

## 📖 基本的な使い方

サンプルファイル `jmbraille-sample.tex` をテンプレートとしてご活用いただけます。

### 1. 墨字（通常印刷用）のコンパイル
オプション指定なしでコンパイルします。
```bash
uplatex filename.tex
dvipdfmx filename.dvi
```

### 2. 点字用データのコンパイル
`\usepackage[braille, ...]{jmbraille}` のように `braille` オプションを指定し、**shell-escape を有効にして**コンパイルします（相互参照のため複数回の実行が必要です）。
```bash
uplatex -shell-escape -8bit filename.tex
dvipdfmx filename.dvi
```

---

## 📂 生成される主なファイル形式

コンパイル成功時、オプションによって以下の各種出力ファイルが作成されます。

| 拡張子 | 説明 |
| :--- | :--- |
| `.nab` | 点字本体ファイル（テキスト形式。点字プリンタ印刷用 / 点字ディスプレイ表示用） |
| `.gnb` | 拡張点字ファイル（行列や場合分けが含まれる場合に自動生成） |
| `.txt` | UTF-8 の 8点点字テキスト（ピンディスプレイ用） |
| `.pdf` | 図や表が含まれる場合、立体コピー作成用に利用 |
| `.edl` | プロッタプリンタ（edel 形式）用ファイル |

---

## ✍️ LaTeX ソース記述時の重要な注意点

点訳は DVI/PDF の見た目ではなく **TeX ソースコードの意味構造** を元に行われます。正しい点訳結果を得るため、以下の点にご注意ください。

1. **意味構造の正確性:**
   - 見た目が同じでもソースコードの意味が異なると点訳結果が変わります。
   - 数式中に日本語を直書きせず、`$\text{日本語}$` のように記述してください。
2. **カスタム記号・マクロ:**
   - 独自の記号やコマンドを使用する場合は、直接書き込まず `\newcommand` でプリアンブルに定義しておくと、後から点字用定義へ差し替えやすくなります。
3. **文脈・モード（通常・数式・情報）の区別:**
   - 点字はモードによって記号や括弧の扱いが変化します。
   - メールアドレスやプログラムソースは `\texttt{...}` や `verbatim` 等の情報モード（情報用点字）を使用してください。
4. **読み指定（`\BrailleReading`）:**
   - 同音異義語や特殊な読み（例：「元」「根」「底」など）で誤訳が生じる場合、`\BrailleReading<もと>{元}` のように指定できます。

---

## ⚙️ 主要パッケージオプション一覧

```latex
\usepackage[braille, windows, cache]{jmbraille}
```

| オプション | 概要 |
| :--- | :--- |
| `braille` | 点字用出力モード |
| `pindisplay` | 全てピンディスプレイ（テキスト）用に出力 |
| `cache` | 一度点訳した文章を保持し、コンパイルを高速化 |
| `windows` / `mac` / `linux` | 実行プラットフォームの指定 |
| `file` | 外部プログラム連携時にファイル経由でやり取りを行うトラブルシューティング用オプション |
| `uebg1` / `uebg2` | 英語を UEB Grade 1 / Grade 2 で点訳 |
