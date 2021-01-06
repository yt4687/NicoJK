﻿/*
 NicoJK
  TVTest ニコニコ実況プラグイン
*/

■なにこれ？
ニコニコ実況を表示するTVTest用のプラグインです。

# このプラグインはjkcnsl( https://github.com/xtne6f/jkcnsl )アプリを介してニコニ
# コのテレビ実況サーバ(live2.nicovideo.jp)と非公式に通信します。サーバに高負荷を
# 与えるような利用や改変をしないでください。


■使い方
NicoJK.tvtpおよびNicoJK.iniをTVTestのPluginフォルダに入れてください。jkcnsl.exe
はTVTest.exeのあるフォルダに入れてください。

ログファイルへの記録機能は、NicoJK.iniのlogfileModeを1か2にして、Pluginsフォルダ
の中に"NicoJK"というフォルダを作っておくと有効になります。ログは"NicoJK"フォルダ
に保存されていきます。録画中に受信した実況コメントがファイル再生プラグイン(何で
もいい)で再生中に表示されればOKです。

■設定
NicoJK.iniを確認してください。

■コメント投稿について
コメント投稿機能を有効にする場合は以下の作業を行ってください:
※DTV板の有志により、ブラウザに依存しないログインツールなども提供されています。
1."sqlite3.exe"を用意
  https://sqlite.org/download.html の「Precompiled Binaries for Windows」から
  "sqlite-tools-win32-x86-{数字}.zip"をダウンロードして、中身の"sqlite3.exe"を
  TVTest.exeのある場所かパスの通った場所(C:\windows あたりがオススメ)に配置
2.必要なら"jq.exe"を用意
  https://stedolan.github.io/jq/ から"jq-win32.exe"か"jq-win64.exe"をダウンロー
  ドして、"jq.exe"にリネームして上記と同様に配置
2.ブラウザでニコニコ実況にログイン
  FirefoxやEdgeやGoogleChromeで https://jk.nicovideo.jp にアクセスして各自のアカ
  ウントでログイン
3.NicoJK.iniのexecGetCookieにブラウザのプロファイルフォルダを設定
  プロファイルフォルダの場所については「firefox cookies.sqlite」や「chrome
  cookie 保存場所」などのキーワードでググって見つけてください。正しく設定すれば
  プラグイン有効時にこのコマンドが実行され、勢い窓にコメント投稿欄が現れます
4.必要ならexecGetV10Keyにもブラウザのプロファイルフォルダを設定
  最近のEdgeやGoogleChromeのクッキー情報はブラウザのマスターキーで保護されている
  ため、この設定も必要です

コメント投稿欄の仕様は以下のとおりです:
・Enterキー押下で投稿
・行頭に@があればローカルコマンドとして処理(詳しくは投稿欄に半角で"@help"と入力)
・行頭に[]で囲われた部分があれば公式サイトのコマンド欄と同等
・投稿欄が空のとき、Ctrl+Vで複数行のペーストができる
・TabまたはRS(レコードセパレータ)文字は改行文字と解釈する
  ・RSは右クリ→「Unicode制御文字の挿入」で入力可能。IMEに辞書登録すると便利
・最長75文字。最短投稿間隔は2秒

専用のアカウントを使う場合は、ログイン用のユーザプロファイルをブラウザに作成する
と便利です。Firefoxの手順:
1.-no-remote -p sub(←プロファイル名、何でもOK)というオプション付きでfirefox.exe
  へのショートカットを作成して起動
2.「ユーザプロファイルの選択」というウィンドウが出るので"sub"という名前の新しい
  プロファイルを作成
3.リストボックスは"default"プロファイルを選択して終了ボタンを押す。以降はショー
  トカットから起動したときに"sub"プロファイルが使われる

■制限
現状、「勢い」情報の取得方法がまとまっていません。差し当たりchannelsUri設定を追
加しましたが、情報提供者の負担などを考えるとこれが最良なのか迷います。
今のところbigコマンドは解釈しません。投稿コマンドは実況サーバによる制限を受ける
場合があります。
パネルのNicoJKタブのテーマ/配色は"操作パネル"の設定に連動します。

■テスト環境
Win7 sp1 + PT2/PT3 + ptTimer + BonDriver_ptmr.dll + TVTest 0.7.19(x86)
※このフォークのテスト環境はWindows10 TVTest 0.10.0-dev(x64)

■配布
http://www.rutice.net/
古いやつは↓
https://github.com/rutice/NicoJK/downloads
リンクする場合は、配布ページか、ソースコードのページへお願いします。
※このフォークの配布ページは https://github.com/xtne6f/NicoJK/releases

■ソースコード
https://github.com/rutice/NicoJK
※このフォークのソースコードは https://github.com/xt4ubq/NicoJK

■ログの仕様(開発者むけ)
ルートフォルダに"jk{実況番号}"というフォルダ(jkフォルダ)を作成する。jkフォルダに
は"{10桁のunix時間(=ログの最初のchatタグのdate属性値)}.txt"というログファイルを
作成する。ログファイル追記中は"lockfile"というファイルをjkフォルダに排他モードで
開いておく。ルートフォルダには(jkフォルダと被らなければ)任意のフォルダやファイル
を置くことができるが、jkフォルダにはログ以外の.txtファイルは置くべきでない。

ログファイルは実況サーバから取得したUTF-8のchatタグを改行CRLFで羅列したもの。
chatタグの要素に改行を含むときはLF=&#10;、CR=&#13;の数値文字参照に置きかえる。
chatタグの左右に空白その他の文字を加えてはいけない(BOMも特別扱いしないので先頭行
にはコメントなど入れておくと安全かも)。chat以外のタグを含んでも構わないが、最終
行はchatタグでなければならない(最終行のdate属性値からログの範囲を求めるため)。

■感謝
過去ログ再生機能、コメント機能を実装していただいたxtne6f氏に感謝いたします。
N/実/勢アイコンは「ふい字」( https://hp.vector.co.jp/authors/VA039499/ )です。

■更新履歴
ChangeLog.txtを参照
