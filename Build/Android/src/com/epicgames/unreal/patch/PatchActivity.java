// Copyright Epic Games, Inc. All Rights Reserved.

// S1 全链入口（docs/android-port-feasibility-2026-11-02.md §一.2）：
// LAUNCHER = 本 Activity（manifest 由 Plugins/UGPAndroidPatch 的 UPL 改写，
// 运行在 :ugppatch 专属进程）。纯 Java 版本检查（本地 index.json 的
// contentVersion vs CDN latest.json 的 currentVersion），需要更新则调
// libugp.so 的 nativeRunPatch —— 此刻引擎从未启动、容器从未挂载。完成后
// 转 GameActivity：引擎完整启动，从项目 Content/Paks 挂载新容器。
//
// InstallDir 公式必须与引擎一致（AndroidJNI.cpp nativeSetGlobalActivity）：
// Development 下 GFilePathBase = getExternalFilesDir(null)，
// BasePath = GFilePathBase + "/UnrealGame/" + <ProjectName> + "/"，
// 挂载点   = BasePath + "<ProjectName>/Content/Paks"。

package com.epicgames.unreal.patch;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

public class PatchActivity extends Activity
{
	private static final String TAG = "UGPPatch";
	private static final String PROJECT_NAME = "IOStoreDemo";
	// 真机经 `adb reverse tcp:18080 tcp:18080` 后，127.0.0.1 即 PC 上的 cdn_server.py。
	private static final String CDN_URL = "http://127.0.0.1:18080";

	/** 与 libugp 的 FJavaEventSink 声明严格一致：onUpdateEvent "(IIZIILjava/lang/String;)V"。 */
	public interface OnUpdateEventListener
	{
		void onUpdateEvent(int phase, int error, boolean isError, int current, int total, String message);
	}

	static
	{
		System.loadLibrary("ugp");
	}

	private static native boolean nativeRunPatch(String cmdline, String cdn, String version, String installDir,
		String internalDir, android.content.res.AssetManager assets, OnUpdateEventListener listener);

	// libugp 的 EUpdatePhase 序数：0=None 1=Manifest 2=Diff 3=Download 4=Apply 5=Commit 6=Done 7=Failed
	private static final String[] PHASES = {"", "清单", "差异", "下载", "写入", "提交", "完成", "失败"};

	private TextView Status;
	private ProgressBar Bar;
	private Button Retry;
	// One long-lived worker thread for ALL patch attempts: libugp keeps the
	// engine alive after a failed run (no PreInit/AppExit cycle), and the
	// engine binds its game-thread identity to the thread that ran PreInit -
	// retrying on a fresh thread would run under a stale game-thread id.
	private Handler Worker;
	private HandlerThread WorkerThread;

	@Override
	public void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);
		LinearLayout root = new LinearLayout(this);
		root.setOrientation(LinearLayout.VERTICAL);
		root.setGravity(Gravity.CENTER);
		root.setPadding(48, 48, 48, 48);
		TextView title = new TextView(this);
		title.setText("IOStoreDemo 更新器");
		title.setTextSize(22);
		title.setGravity(Gravity.CENTER);
		Status = new TextView(this);
		Status.setText("检查版本…");
		Status.setTextSize(15);
		Bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
		Bar.setMax(1000);
		Retry = new Button(this);
		Retry.setText("重试");
		Retry.setOnClickListener(v -> {
			Retry.setEnabled(false);
			Retry.setVisibility(LinearLayout.GONE);
			setStatus("重试…");
			Worker.post(this::runFlow);
		});
		Retry.setVisibility(LinearLayout.GONE);
		root.addView(title);
		root.addView(Status);
		root.addView(Bar);
		root.addView(Retry);
		setContentView(root);
		WorkerThread = new HandlerThread("UGPPatchFlow");
		WorkerThread.start();
		Worker = new Handler(WorkerThread.getLooper());
		Worker.post(this::runFlow);
	}

	@Override
	protected void onDestroy()
	{
		// The engine (kept alive by libugp on failure) outlives this activity;
		// only the worker looper is ours to stop.
		if (WorkerThread != null)
		{
			WorkerThread.quitSafely();
			WorkerThread = null;
		}
		super.onDestroy();
	}

	private void runFlow()
	{
		// Engine BasePath = <files>/UnrealGame/<ProjectName>/ (GFilePathBase +
		// FILEBASE_DIRECTORY) - the sandbox the engine scans for <ProjectName>/Content/Paks
		// and reads loose staged files from. UGP installs the WHOLE staged
		// tree here (recursive ingest, ids = relative paths).
		File installDir = new File(getExternalFilesDir(null),
			"UnrealGame/" + PROJECT_NAME);

		String local = readLocalVersion(installDir);
		String remote = fetchRemoteVersion();
		if (remote == null)
		{
			if (local == null)
			{
				showFailure("无法获取 CDN 版本，首次安装需要联网");
			}
			else
			{
				toGame("无法获取 CDN 版本，启动已安装版本");
			}
			return;
		}
		if (remote.equals(local))
		{
			toGame("已是最新版本 " + remote);
			return;
		}

		final String target = remote;
		setStatus("更新 " + (local == null ? "(未安装)" : local) + " -> " + target);
		Log.i(TAG, "start patch: install=" + installDir + " target=" + target);
		boolean ok = nativeRunPatch("-NoPak", CDN_URL, target, installDir.getAbsolutePath(),
			getFilesDir().getAbsolutePath(), getAssets(),
			(phase, error, isError, cur, total, msg) ->
			{
				String phaseName = (phase >= 1 && phase < PHASES.length) ? PHASES[phase] : ("阶段" + phase);
				if (isError)
				{
					setStatus("[" + phaseName + "] 错误 " + error + ": " + msg);
				}
				else if (total > 0)
				{
					Bar.setProgress((int) (1000L * cur / total));
					setStatus("[" + phaseName + "] " + cur + "/" + total + (msg.isEmpty() ? "" : (" " + msg)));
				}
				else
				{
					setStatus("[" + phaseName + "] " + msg);
				}
			});
		if (ok)
		{
			// Preserve the staged UECommandLine.txt and the engine's relative project paths.
			toGame("更新完成 -> " + target);
		}
		else
		{
		// 更新失败：停驻在本界面（不进游戏），给出重试入口。
		showFailure("更新失败（详见 logcat UGPPatch）");
		}
	}

	/** 失败停驻：不清场、不转 GameActivity，停在错误信息 + 重试按钮。 */
	private void showFailure(final String message)
	{
		Log.e(TAG, message);
		runOnUiThread(() ->
		{
			Bar.setVisibility(LinearLayout.GONE);
			Status.setText(message);
			Status.setTextColor(Color.RED);
			Retry.setVisibility(LinearLayout.VISIBLE);
			Retry.setEnabled(true);
		});
	}

	private void toGame(String message)
	{
		setStatus(message);
		Log.i(TAG, message);
		try
		{
			Thread.sleep(1200);
		}
		catch (InterruptedException ignored)
		{
		}
		startActivity(new Intent(this, com.epicgames.unreal.GameActivity.class));
		finish();
	}

	private void setStatus(final String s)
	{
		runOnUiThread(() -> Status.setText(s));
	}

	/** 本地版本：<InstallDir>/.ugp/index.json 的 contentVersion（未安装 = null）。 */
	private String readLocalVersion(File installDir)
	{
		try
		{
			File index = new File(installDir, ".ugp/index.json");
			if (!index.isFile())
			{
				return null;
			}
			JSONObject o = new JSONObject(readAll(new FileInputStream(index)));
			return o.optString("contentVersion", null);
		}
		catch (Throwable t)
		{
			Log.w(TAG, "readLocalVersion", t);
			return null;
		}
	}

	/** CDN 版本：GET <cdn>/index/latest.json 的 currentVersion。 */
	private String fetchRemoteVersion()
	{
		HttpURLConnection conn = null;
		try
		{
			conn = (HttpURLConnection) new URL(CDN_URL + "/index/latest.json").openConnection();
			conn.setConnectTimeout(3000);
			conn.setReadTimeout(5000);
			InputStream in = conn.getResponseCode() == 200 ? conn.getInputStream() : conn.getErrorStream();
			if (in == null)
			{
				return null;
			}
			JSONObject o = new JSONObject(readAll(in));
			return o.optString("currentVersion", null);
		}
		catch (Throwable t)
		{
			Log.w(TAG, "fetchRemoteVersion", t);
			return null;
		}
		finally
		{
			if (conn != null)
			{
				conn.disconnect();
			}
		}
	}

	private static String readAll(InputStream in) throws Exception
	{
		ByteArrayOutputStream out = new ByteArrayOutputStream();
		byte[] buf = new byte[8192];
		int n;
		while ((n = in.read(buf)) > 0)
		{
			out.write(buf, 0, n);
		}
		in.close();
		return new String(out.toByteArray(), StandardCharsets.UTF_8);
	}
}
